#define _POSIX_C_SOURCE 200809L
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "ipc.h"
#include "runtime_adapters.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static voice_controller *app;
static pthread_mutex_t row_mu = PTHREAD_MUTEX_INITIALIZER;
static char **rows;
static size_t row_count, row_cap;
static char **errors;
static size_t error_count, error_cap;
static char **reads;
static size_t read_count, read_cap;
static char **audio_calls;
static size_t audio_count, audio_cap;
static char *restored;
static char *hold_bridge;
static pthread_mutex_t hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cv = PTHREAD_COND_INITIALIZER;
static int released;
static int hold_first;
static char pane[256];
static char herdr_socket[4096];
static volatile sig_atomic_t stop_requested;
static void handle_client(int client);

static void on_term(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void push(char ***list, size_t *count, size_t *cap, const char *text) {
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *list = realloc(*list, *cap * sizeof **list);
    }
    (*list)[(*count)++] = strdup(text ? text : "");
}

static int guarded_herdr(void *user, const controller_target_view *target, const char *const *argv, char **result_json, char *err, size_t cap) {
    (void)user;
    if (!target || strcmp(target->pane, pane) != 0 || strcmp(target->socket_path, herdr_socket) != 0) {
        snprintf(err, cap, "Foreign Herdr pane");
        return CTRL_ERR;
    }
    int allowed = argv && ((strcmp(argv[0], "pane") == 0 && argv[1] && strcmp(argv[1], "process-info") == 0)
        || (strcmp(argv[0], "api") == 0 && argv[1] && strcmp(argv[1], "snapshot") == 0));
    if (!allowed) {
        snprintf(err, cap, "Forbidden Herdr operation");
        return CTRL_ERR;
    }
    voice_target_fields fields = {0};
    fields.socket = target->socket_path;
    fields.pane = target->pane;
    fields.harness = target->harness;
    fields.pid = target->pid;
    fields.managed = target->managed;
    fields.start = target->start;
    voice_target *owned = voice_target_new(&fields);
    int argc = 0;
    while (argv[argc]) argc++;
    yyjson_doc *result = NULL;
    voice_herdr *herdr = voice_herdr_new(NULL);
    int rc = voice_herdr_request(herdr, owned, argv, argc, 0, &result, err, cap);
    voice_herdr_free(herdr);
    voice_target_free(owned);
    if (rc != VOICE_OK || !result) return CTRL_ERR;
    *result_json = yyjson_val_write(yyjson_doc_get_root(result), 0, NULL);
    yyjson_mut_doc *row = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(row);
    yyjson_mut_doc_set_root(row, root);
    yyjson_mut_val *command = yyjson_mut_arr(row);
    for (int i = 0; i < argc; i++) yyjson_mut_arr_add_strcpy(row, command, argv[i]);
    yyjson_mut_obj_add_val(row, root, "command", command);
    yyjson_mut_obj_add_val(row, root, "result", yyjson_val_mut_copy(row, yyjson_doc_get_root(result)));
    char *encoded = yyjson_mut_write(row, 0, NULL);
    pthread_mutex_lock(&row_mu);
    push(&reads, &read_count, &read_cap, encoded);
    pthread_mutex_unlock(&row_mu);
    free(encoded);
    yyjson_mut_doc_free(row);
    yyjson_doc_free(result);
    return CTRL_OK;
}

static int silent_status(void *user, char **json) {
    (void)user;
    *json = strdup("{}");
    return 0;
}
static void silent_stop(void *user) { (void)user; }
static int unexpected_audio(const char *name) {
    pthread_mutex_lock(&row_mu);
    push(&audio_calls, &audio_count, &audio_cap, name);
    pthread_mutex_unlock(&row_mu);
    return CTRL_ERR;
}
static int silent_cue(void *user, int frequency, char *err, size_t cap) {
    (void)user; (void)frequency;
    unexpected_audio("cue");
    snprintf(err, cap, "Unexpected audio operation: cue");
    return CTRL_ERR;
}
static controller_capture *silent_capture(void *user, const char *path, char *err, size_t cap) {
    (void)user; (void)path;
    unexpected_audio("start_capture");
    snprintf(err, cap, "Unexpected audio operation: start_capture");
    return NULL;
}
static int silent_transcribe(void *user, const char *path, atomic_int *cancelled, void (*on_drained)(const char *, void *), void (*release)(void *), void *drain, char *out, size_t out_cap, char *err, size_t cap) {
    (void)user; (void)path; (void)cancelled; (void)on_drained; (void)out; (void)out_cap;
    if (release) release(drain);
    unexpected_audio("transcribe");
    snprintf(err, cap, "Unexpected audio operation: transcribe");
    return CTRL_ERR;
}
static int silent_speak(void *user, const char *text, atomic_int *cancelled, char *err, size_t cap) {
    (void)user; (void)text; (void)cancelled;
    unexpected_audio("speak");
    snprintf(err, cap, "Unexpected audio operation: speak");
    return CTRL_ERR;
}

static char *json_array(char **items, size_t count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);
    for (size_t i = 0; i < count; i++) {
        yyjson_doc *item = yyjson_read(items[i], strlen(items[i]), 0);
        if (item) yyjson_mut_arr_add_val(arr, yyjson_val_mut_copy(doc, yyjson_doc_get_root(item)));
        else yyjson_mut_arr_add_strcpy(doc, arr, items[i]);
        yyjson_doc_free(item);
    }
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return text;
}

static char *fixture_state(void) {
    pthread_mutex_lock(&row_mu);
    char *row_json = json_array(rows, row_count);
    char *error_json = json_array(errors, error_count);
    char *read_json = json_array(reads, read_count);
    char *audio_json = json_array(audio_calls, audio_count);
    pthread_mutex_unlock(&row_mu);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", 1);
    yyjson_doc *rows_doc = yyjson_read(row_json, strlen(row_json), 0);
    yyjson_doc *errors_doc = yyjson_read(error_json, strlen(error_json), 0);
    yyjson_doc *reads_doc = yyjson_read(read_json, strlen(read_json), 0);
    yyjson_doc *audio_doc = yyjson_read(audio_json, strlen(audio_json), 0);
    yyjson_doc *restored_doc = restored ? yyjson_read(restored, strlen(restored), 0) : NULL;
    yyjson_mut_obj_add_val(doc, root, "rows", yyjson_val_mut_copy(doc, yyjson_doc_get_root(rows_doc)));
    yyjson_mut_obj_add_val(doc, root, "errors", yyjson_val_mut_copy(doc, yyjson_doc_get_root(errors_doc)));
    yyjson_mut_obj_add_val(doc, root, "reads", yyjson_val_mut_copy(doc, yyjson_doc_get_root(reads_doc)));
    yyjson_mut_obj_add_val(doc, root, "audio_calls", yyjson_val_mut_copy(doc, yyjson_doc_get_root(audio_doc)));
    yyjson_mut_obj_add_val(doc, root, "restored", restored_doc ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(restored_doc)) : yyjson_mut_arr(doc));
    pthread_mutex_lock(&app->state);
    yyjson_mut_val *sessions = yyjson_mut_arr(doc);
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
        session_entry *entry = &app->sessions[i];
        if (!entry->used || !entry->target) continue;
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, row, "token", entry->token);
        char *selection = controller_selection_json(app);
        (void)selection;
        yyjson_mut_val *target = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, target, "pane", entry->target->data.pane);
        yyjson_mut_obj_add_strcpy(doc, target, "socket", entry->target->data.socket);
        yyjson_mut_obj_add_int(doc, target, "pid", entry->target->data.pid);
        if (entry->target->data.has_start) yyjson_mut_obj_add_strcpy(doc, target, "start", entry->target->data.start);
        yyjson_mut_obj_add_strcpy(doc, target, "harness", entry->target->data.harness);
        if (entry->target->data.has_session && !entry->target->data.session_null)
            yyjson_mut_obj_add_strcpy(doc, target, "session", entry->target->data.session);
        if (entry->target->data.has_bridge) yyjson_mut_obj_add_strcpy(doc, target, "bridge_id", entry->target->data.bridge_id);
        if (entry->target->data.has_activation) yyjson_mut_obj_add_sint(doc, target, "activation", entry->target->data.activation);
        if (entry->target->data.has_adapter) yyjson_mut_obj_add_strcpy(doc, target, "adapter_socket", entry->target->data.adapter_socket);
        yyjson_mut_obj_add_bool(doc, target, "managed", entry->target->data.managed);
        yyjson_mut_obj_add_bool(doc, target, "team_child", entry->target->data.team_child);
        yyjson_mut_obj_add_val(doc, row, "target", target);
        yyjson_mut_obj_add_bool(doc, row, "ready", entry->has_connection && strcmp(entry->connection_state, "ready") == 0);
        yyjson_mut_obj_add_real(doc, row, "heartbeat_at", entry->heartbeat_at);
        yyjson_mut_arr_add_val(sessions, row);
        free(selection);
    }
    pthread_mutex_unlock(&app->state);
    yyjson_mut_obj_add_val(doc, root, "sessions", sessions);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(rows_doc);
    yyjson_doc_free(errors_doc);
    yyjson_doc_free(reads_doc);
    yyjson_doc_free(audio_doc);
    yyjson_doc_free(restored_doc);
    free(row_json);
    free(error_json);
    free(read_json);
    free(audio_json);
    return text;
}

static void *client_thread(void *arg) {
    int client = *(int *)arg;
    free(arg);
    handle_client(client);
    return NULL;
}

static void handle_client(int client) {
    char buf[VOICE_REQUEST_MAX + 1];
    size_t n = 0;
    while (n < VOICE_REQUEST_MAX) {
        ssize_t got = read(client, buf + n, sizeof buf - 1 - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += (size_t)got;
        if (memchr(buf, '\n', n)) break;
    }
    buf[n] = 0;
    char *nl = memchr(buf, '\n', n);
    char *response = NULL;
    if (!nl) response = voice_envelope_error("Voice request exceeds the size limit");
    else {
        *nl = 0;
        yyjson_doc *doc = yyjson_read(buf, strlen(buf), 0);
        const char *action = doc && yyjson_is_str(yyjson_obj_get(yyjson_doc_get_root(doc), "action"))
            ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "action")) : "";
        if (strcmp(action, "fixture-release") == 0) {
            pthread_mutex_lock(&hold_mu);
            released = 1;
            pthread_cond_broadcast(&hold_cv);
            pthread_mutex_unlock(&hold_mu);
            response = strdup("{\"ok\":true}");
        } else if (strcmp(action, "fixture-state") == 0) {
            response = fixture_state();
        } else {
            pthread_mutex_lock(&row_mu);
            push(&rows, &row_count, &row_cap, buf);
            pthread_mutex_unlock(&row_mu);
            if (strcmp(action, "attach") == 0 && hold_first) {
                yyjson_val *target = yyjson_obj_get(yyjson_doc_get_root(doc), "target");
                const char *bridge = yyjson_is_str(yyjson_obj_get(target, "bridge_id")) ? yyjson_get_str(yyjson_obj_get(target, "bridge_id")) : "";
                int wait = 0;
                pthread_mutex_lock(&hold_mu);
                if (!hold_bridge) hold_bridge = strdup(bridge);
                wait = hold_bridge && strcmp(hold_bridge, bridge) == 0;
                if (wait && !released) {
                    struct timespec ts;
                    clock_gettime(CLOCK_REALTIME, &ts);
                    ts.tv_sec += 90;
                    while (!released && !stop_requested) {
                        if (pthread_cond_timedwait(&hold_cv, &hold_mu, &ts) == ETIMEDOUT) break;
                    }
                }
                int timed_out = wait && !released;
                pthread_mutex_unlock(&hold_mu);
                if (timed_out) {
                    pthread_mutex_lock(&row_mu);
                    push(&errors, &error_count, &error_cap, "RuntimeError: Held attachment was not released");
                    pthread_mutex_unlock(&row_mu);
                    response = voice_envelope_error("Held attachment was not released");
                }
            }
            if (!response) response = controller_dispatch(app, buf);
        }
        yyjson_doc_free(doc);
    }
    if (response) {
        size_t length = strlen(response);
        if (write(client, response, length) != (ssize_t)length ||
            write(client, "\n", 1) != 1)
            fputs("fixture response write failed\n", stderr);
        free(response);
    }
    close(client);
}

int main(int argc, char **argv) {
    const char *runtime = NULL;
    snprintf(pane, sizeof pane, "offline-no-pane");
    snprintf(herdr_socket, sizeof herdr_socket, "offline-no-socket");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) runtime = argv[++i];
        else if (strcmp(argv[i], "--pane") == 0 && i + 1 < argc) snprintf(pane, sizeof pane, "%s", argv[++i]);
        else if (strcmp(argv[i], "--herdr-socket") == 0 && i + 1 < argc) snprintf(herdr_socket, sizeof herdr_socket, "%s", argv[++i]);
        else if (strcmp(argv[i], "--hold-first") == 0) hold_first = 1;
    }
    if (!runtime) return 2;
    if (!hold_first) released = 1;
    controller_deps deps = {0};
    deps.audio.status_json = silent_status;
    deps.audio.stop = silent_stop;
    deps.audio.cue = silent_cue;
    deps.audio.start_capture = silent_capture;
    deps.audio.transcribe_owned = silent_transcribe;
    deps.audio.speak = silent_speak;
    deps.herdr.request = guarded_herdr;
    deps.speech_available = 1;
    app = controller_create(runtime, &deps);
    if (!app) return 1;
    controller_restore(app);
    pthread_mutex_lock(&app->state);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, arr);
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
        if (!app->sessions[i].used) continue;
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, row, "token", app->sessions[i].token);
        yyjson_mut_obj_add_strcpy(doc, row, "state", app->sessions[i].has_connection ? app->sessions[i].connection_state : "ready");
        yyjson_mut_val *target = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, target, "pane", app->sessions[i].target->data.pane);
        yyjson_mut_obj_add_strcpy(doc, target, "bridge_id", app->sessions[i].target->data.bridge_id);
        yyjson_mut_obj_add_val(doc, row, "target", target);
        yyjson_mut_arr_add_val(arr, row);
    }
    pthread_mutex_unlock(&app->state);
    restored = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    int fd = -1;
    char err[256];
    if (voice_bind_control(runtime, &fd, err, sizeof err) != 0) return 1;
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    printf("ready\n");
    fflush(stdout);
    while (!stop_requested) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) <= 0) continue;
        int client = accept(fd, NULL, NULL);
        if (client < 0) continue;
        pthread_t thread;
        int *owned = malloc(sizeof *owned);
        if (!owned) {
            close(client);
            continue;
        }
        *owned = client;
        if (pthread_create(&thread, NULL, client_thread, owned) != 0) {
            handle_client(client);
            free(owned);
            continue;
        }
        pthread_detach(thread);
    }
    close(fd);
    controller_shutdown(app);
    controller_free(app);
    return 0;
}
