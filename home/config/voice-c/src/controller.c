#define _POSIX_C_SOURCE 200809L
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "text.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

double controller_now(voice_controller *app) {
    if (app->deps.clock) return app->deps.clock(app->deps.clock_user);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *pool_main(void *arg) {
    voice_controller *app = arg;
    for (;;) {
        pthread_mutex_lock(&app->pool_mu);
        while (!app->pool_stop && app->q_count == 0) pthread_cond_wait(&app->pool_cv, &app->pool_mu);
        if (app->pool_stop && app->q_count == 0) {
            pthread_mutex_unlock(&app->pool_mu);
            return NULL;
        }
        ctrl_job job = app->queue[app->q_head];
        app->q_head = (app->q_head + 1) % CTRL_QUEUE;
        app->q_count--;
        pthread_cond_signal(&app->pool_cv);
        pthread_mutex_unlock(&app->pool_mu);
        job.fn(job.arg);
    }
}

static int pool_start(voice_controller *app) {
    pthread_mutex_init(&app->pool_mu, NULL);
    pthread_cond_init(&app->pool_cv, NULL);
    for (int i = 0; i < CTRL_WORKERS; i++) {
        if (pthread_create(&app->workers[i], NULL, pool_main, app) != 0) return -1;
    }
    app->pool_started = CTRL_WORKERS;
    return 0;
}

int controller_submit(voice_controller *app, void (*fn)(void *), void *arg) {
    pthread_mutex_lock(&app->pool_mu);
    if (app->pool_stop || app->q_count == CTRL_QUEUE) {
        pthread_mutex_unlock(&app->pool_mu);
        return -1;
    }
    app->queue[app->q_tail].fn = fn;
    app->queue[app->q_tail].arg = arg;
    app->q_tail = (app->q_tail + 1) % CTRL_QUEUE;
    app->q_count++;
    pthread_cond_signal(&app->pool_cv);
    pthread_mutex_unlock(&app->pool_mu);
    return 0;
}

static void pool_shutdown(voice_controller *app) {
    pthread_mutex_lock(&app->pool_mu);
    app->pool_stop = 1;
    pthread_cond_broadcast(&app->pool_cv);
    pthread_mutex_unlock(&app->pool_mu);
    for (int i = 0; i < app->pool_started; i++) pthread_join(app->workers[i], NULL);
    app->pool_started = 0;
}

voice_controller *controller_create(const char *runtime_dir, const controller_deps *deps) {
    if (!runtime_dir || !runtime_dir[0] || strlen(runtime_dir) >= CTRL_PATH) return NULL;
    voice_controller *app = calloc(1, sizeof *app);
    if (!app) return NULL;
    atomic_init(&app->attachments_closed, 0);
    app->runtime = strdup(runtime_dir);
    if (!app->runtime) {
        free(app);
        return NULL;
    }
    if (deps) app->deps = *deps;
    pthread_mutex_init(&app->state, NULL);
    pthread_mutex_init(&app->delivery, NULL);
    pthread_mutex_init(&app->drain_mu, NULL);
    app->attachments = registry_new();
    snprintf(app->phase, sizeof app->phase, "idle");
    app->speech_available = deps ? deps->speech_available : 1;
    app->auto_read = deps && deps->auto_speak && app->speech_available;
    app->input_op = controller_op_new(app);
    app->record_op = controller_op_new(app);
    app->speak_op = controller_op_new(app);
    if (!app->attachments || !app->input_op || !app->record_op || !app->speak_op || pool_start(app) != 0) {
        controller_free(app);
        return NULL;
    }
    return app;
}

void controller_mark_audible(voice_controller *app) {
    if (!app) return;
    pthread_mutex_lock(&app->state);
    app->audible = 1;
    pthread_mutex_unlock(&app->state);
}

target_snap *controller_snap(const target_data *data) {
    target_snap *snap = calloc(1, sizeof *snap);
    if (!snap) return NULL;
    atomic_init(&snap->refs, 1);
    snap->data = *data;
    return snap;
}

target_snap *controller_snap_retain(target_snap *snap) {
    if (snap) atomic_fetch_add(&snap->refs, 1);
    return snap;
}

void controller_snap_release(target_snap *snap) {
    if (snap && atomic_fetch_sub(&snap->refs, 1) == 1) free(snap);
}

void controller_view(const target_data *data, controller_target_view *out) {
    memset(out, 0, sizeof *out);
    out->pane = data->pane;
    out->socket_path = data->socket;
    out->has_socket_instance = data->has_socket_instance;
    out->socket_device = data->socket_device;
    out->socket_inode = data->socket_inode;
    out->pid = data->pid;
    out->start = data->has_start ? data->start : NULL;
    out->harness = data->harness;
    out->session = data->session_null ? NULL : data->session;
    out->has_session = data->has_session && !data->session_null;
    out->bridge_id = data->has_bridge ? data->bridge_id : NULL;
    out->has_activation = data->has_activation;
    out->activation = data->activation;
    out->team_child = data->team_child;
    out->has_team_child = data->has_team_child;
    out->model = data->has_model ? data->model : NULL;
    out->thinking = data->has_thinking ? data->thinking : NULL;
    out->adapter_socket = data->has_adapter ? data->adapter_socket : NULL;
    out->has_adapter = data->has_adapter;
    out->adapter_device = data->adapter_device;
    out->adapter_inode = data->adapter_inode;
    out->managed = data->managed;
    out->token = data->token;
}

int controller_same_attachment(const target_data *a, const target_data *b) {
    if (!a || !b) return 0;
    if (strcmp(a->pane, b->pane) != 0 || strcmp(a->socket, b->socket) != 0 || a->pid != b->pid) return 0;
    if (a->has_start && b->has_start && strcmp(a->start, b->start) != 0) return 0;
    if (strcmp(a->harness, b->harness) != 0 || a->managed != b->managed || a->team_child != b->team_child) return 0;
    if (a->has_bridge != b->has_bridge || (a->has_bridge && strcmp(a->bridge_id, b->bridge_id) != 0)) return 0;
    if (a->has_activation != b->has_activation || (a->has_activation && a->activation != b->activation)) return 0;
    if (a->has_adapter && b->has_adapter && (strcmp(a->adapter_socket, b->adapter_socket) != 0
            || a->adapter_device != b->adapter_device || a->adapter_inode != b->adapter_inode)) return 0;
    if (a->has_session && b->has_session && !a->session_null && !b->session_null && strcmp(a->session, b->session) != 0) return 0;
    return 1;
}

int controller_same_target(const target_data *a, const target_data *b) {
    if (!a || !b) return 0;
    return strcmp(a->pane, b->pane) == 0 && strcmp(a->socket, b->socket) == 0 && a->pid == b->pid
        && a->has_start == b->has_start && (!a->has_start || strcmp(a->start, b->start) == 0)
        && strcmp(a->harness, b->harness) == 0
        && a->has_socket_instance == b->has_socket_instance
        && (!a->has_socket_instance || (a->socket_device == b->socket_device && a->socket_inode == b->socket_inode))
        && a->has_adapter == b->has_adapter
        && (!a->has_adapter || (strcmp(a->adapter_socket, b->adapter_socket) == 0
            && a->adapter_device == b->adapter_device && a->adapter_inode == b->adapter_inode))
        && a->has_bridge == b->has_bridge && (!a->has_bridge || strcmp(a->bridge_id, b->bridge_id) == 0)
        && a->has_activation == b->has_activation && (!a->has_activation || a->activation == b->activation)
        && a->managed == b->managed && a->team_child == b->team_child
        && strcmp(a->token, b->token) == 0;
}

op_state *controller_op_new(voice_controller *app) {
    op_state *op = calloc(1, sizeof *op);
    if (!op) return NULL;
    atomic_init(&op->refs, 1);
    atomic_init(&op->cancelled, 0);
    op->generation = ++app->next_generation;
    op->recovery_revision = app->recovery_revision;
    return op;
}

op_state *controller_op_retain(op_state *op) {
    if (op) atomic_fetch_add(&op->refs, 1);
    return op;
}

void controller_op_release(op_state *op) {
    if (op && atomic_fetch_sub(&op->refs, 1) == 1) free(op);
}

session_entry *controller_find(voice_controller *app, const char *token) {
    if (!token || !token[0]) return NULL;
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
        if (app->sessions[i].used && strcmp(app->sessions[i].token, token) == 0) return &app->sessions[i];
    }
    return NULL;
}

int controller_has_turn(char **turns, size_t count, const char *id) {
    if (!id) return 0;
    for (size_t i = 0; i < count; i++) if (turns && turns[i] && strcmp(turns[i], id) == 0) return 1;
    return 0;
}

int controller_add_turn(char ***turns, size_t *count, size_t *cap, const char *id) {
    if (!id || !id[0] || controller_has_turn(*turns, *count, id)) return 0;
    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        char **grown = realloc(*turns, ncap * sizeof *grown);
        if (!grown) return -1;
        *turns = grown;
        *cap = ncap;
    }
    (*turns)[*count] = strdup(id);
    if (!(*turns)[*count]) return -1;
    (*count)++;
    return 1;
}

void controller_clear_turns(char ***turns, size_t *count, size_t *cap) {
    if (turns && *turns) {
        for (size_t i = 0; i < *count; i++) free((*turns)[i]);
        free(*turns);
    }
    if (turns) *turns = NULL;
    if (count) *count = 0;
    if (cap) *cap = 0;
}

int controller_copy_turns(char **src, size_t n, char ***dst, size_t *count, size_t *cap) {
    controller_clear_turns(dst, count, cap);
    for (size_t i = 0; i < n; i++) if (controller_add_turn(dst, count, cap, src[i]) < 0) return -1;
    return 0;
}

static void session_clear(session_entry *entry) {
    if (!entry) return;
    controller_snap_release(entry->target);
    free(entry->pending);
    free(entry->reply);
    controller_clear_turns(&entry->turns, &entry->turn_count, &entry->turn_cap);
    socket_key_clear(&entry->socket_key);
    memset(entry, 0, sizeof *entry);
    snprintf(entry->agent_state, sizeof entry->agent_state, "unknown");
}

void controller_population(voice_controller *app) {
    if (!app->deps.has_engines || !app->deps.engines.set_population) return;
    int n = 0;
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) if (app->sessions[i].used) n++;
    app->deps.engines.set_population(app->deps.engines.user, n, app->pending_admissions);
}

void controller_remember(voice_controller *app) {
    session_entry *entry = controller_find(app, app->token);
    if (!entry) return;
    entry->has_thread = app->has_thread;
    if (app->has_thread) snprintf(entry->thread, sizeof entry->thread, "%s", app->thread);
    controller_copy_turns(app->turns, app->turn_count, &entry->turns, &entry->turn_count, &entry->turn_cap);
    free(entry->pending);
    entry->pending = app->pending ? strdup(app->pending) : NULL;
    entry->draft = app->draft;
    free(entry->reply);
    entry->reply = app->reply ? strdup(app->reply) : NULL;
}

void controller_load_selected(voice_controller *app, const char *token, int explicit_sel) {
    session_entry *entry = controller_find(app, token);
    if (!entry || !entry->target) return;
    snprintf(app->token, sizeof app->token, "%s", token);
    app->has_token = 1;
    controller_snap_release(app->target);
    app->target = controller_snap_retain(entry->target);
    app->has_thread = entry->has_thread;
    if (entry->has_thread) snprintf(app->thread, sizeof app->thread, "%s", entry->thread);
    else app->thread[0] = 0;
    controller_copy_turns(entry->turns, entry->turn_count, &app->turns, &app->turn_count, &app->turn_cap);
    free(app->pending);
    app->pending = entry->pending ? strdup(entry->pending) : NULL;
    app->draft = entry->draft;
    free(app->reply);
    app->reply = entry->reply ? strdup(entry->reply) : NULL;
    controller_op_release(app->input_op);
    controller_op_release(app->record_op);
    controller_op_release(app->speak_op);
    app->input_op = controller_op_new(app);
    app->record_op = controller_op_new(app);
    app->speak_op = controller_op_new(app);
    app->selection_explicit = explicit_sel;
    snprintf(app->phase, sizeof app->phase, "%s", (app->pending || app->draft) ? "draft" : "idle");
    free(app->error);
    app->error = NULL;
}

void controller_require_ready(voice_controller *app, const char *token, char *err, size_t cap) {
    session_entry *entry = controller_find(app, token);
    if (!entry || !entry->target || !entry->target->data.managed) return;
    if (!entry->has_connection || strcmp(entry->connection_state, "ready") != 0
        || controller_now(app) - entry->heartbeat_at >= LEASE_SECONDS) {
        snprintf(err, cap, "Pi voice is reconnecting; wait for Ready");
    }
}

static void set_reconnect(voice_controller *app, const target_data *target, int explicit_sel) {
    app->has_reconnect = 1;
    snprintf(app->reconnect_path, sizeof app->reconnect_path, "%s", target->socket);
    snprintf(app->reconnect_pane_id, sizeof app->reconnect_pane_id, "%s", target->pane);
    app->reconnect_pane.socket_path = app->reconnect_path;
    app->reconnect_pane.socket_device = target->socket_device;
    app->reconnect_pane.socket_inode = target->socket_inode;
    app->reconnect_pane.pane_id = app->reconnect_pane_id;
    app->reconnect_explicit = explicit_sel;
}

int controller_retain(voice_controller *app, const char *text, const char *token, const target_data *target, op_state *cancelled) {
    if (app->retained || !text || !text[0] || !target) return 0;
    if (strcmp(target->harness, "pi") != 0 && strcmp(target->harness, "qwen-pi") != 0) return 0;
    if (has_control_characters(text) || strlen(text) > MAX_RETAINED_BYTES) return 0;
    if (cancelled && (cancelled->recovery_revision != app->recovery_revision
            || (atomic_load(&cancelled->cancelled) && !cancelled->voice_target_lost))) return 0;
    app->recovery_uncertain = 0;
    retained_dictation *kept = calloc(1, sizeof *kept);
    if (!kept) return 0;
    kept->text = strdup(text);
    if (!kept->text) {
        free(kept);
        return 0;
    }
    snprintf(kept->source_token, sizeof kept->source_token, "%s", token ? token : "");
    snprintf(kept->source_session, sizeof kept->source_session, "%s", target->session_null ? "" : target->session);
    const char *label_harness = target->harness[0] ? target->harness : "pi";
    size_t label_cap = sizeof kept->source_label;
    size_t hlen = strlen(label_harness);
    if (hlen + 3 >= label_cap) hlen = label_cap > 3 ? label_cap - 3 : 0;
    memcpy(kept->source_label, label_harness, hlen);
    memcpy(kept->source_label + hlen, ": ", 2);
    size_t used = hlen + 2;
    size_t plen = strlen(target->pane);
    size_t take = plen < label_cap - 1 - used ? plen : label_cap - 1 - used;
    memcpy(kept->source_label + used, target->pane, take);
    kept->source_label[used + take] = 0;
    app->retained = kept;
    return 1;
}

static void osd_apply(voice_controller *app, const char *message, double seconds, const char *tone);
static void *stop_held(voice_controller *app, int discard, int target_lost);
static void finish_capture(voice_controller *app, void *owner);

int controller_remove_session(voice_controller *app, const char *token, int retire) {
    session_entry *entry = controller_find(app, token);
    if (!entry) return 0;
    int selected = app->has_token && strcmp(app->token, token) == 0;
    if (selected) controller_remember(app);
    target_data target = entry->target->data;
    if (strcmp(target.harness, "pi") == 0 || strcmp(target.harness, "qwen-pi") == 0)
        controller_retain(app, entry->pending, token, &target, NULL);
    if (selected) {
        if (target.managed) set_reconnect(app, &target, app->selection_explicit);
        if (controller_recording_active(app) && app->record_op) {
            app->record_op->voice_target_lost = 1;
            fprintf(stderr, "Pi voice: Recording cancelled: Its voice destination disconnected\n");
            osd_apply(app, "Recording cancelled: Its voice destination disconnected", 6, "orange");
        }
        void *owner = stop_held(app, 1, 1);
        pthread_mutex_unlock(&app->state);
        finish_capture(app, owner);
        pthread_mutex_lock(&app->state);
        app->has_token = 0;
        app->token[0] = 0;
        controller_snap_release(app->target);
        app->target = NULL;
        app->has_thread = 0;
        app->thread[0] = 0;
        free(app->reply);
        app->reply = NULL;
        free(app->pending);
        app->pending = NULL;
        controller_clear_turns(&app->turns, &app->turn_count, &app->turn_cap);
        app->draft = 0;
        snprintf(app->phase, sizeof app->phase, "idle");
    }
    registry_remove(app->attachments, token, retire);
    session_clear(entry);
    controller_population(app);
    return 1;
}

void controller_notice(voice_controller *app, const char *title, const char *detail, const char *tone) {
    char text[VOICE_NOTICE_MAX + 1];
    char extra[VOICE_NOTICE_MAX + 1];
    voice_public_text(title, text, sizeof text);
    voice_public_text(detail, extra, sizeof extra);
    if (extra[0]) {
        char joined[VOICE_NOTICE_MAX * 2 + 4];
        if (text[0]) {
            memcpy(joined, text, strlen(text));
            memcpy(joined + strlen(text), ": ", 2);
            memcpy(joined + strlen(text) + 2, extra, strlen(extra) + 1);
        } else memcpy(joined, extra, strlen(extra) + 1);
        voice_public_text(joined, text, sizeof text);
    }
    if (text[0]) fprintf(stderr, "Pi voice: %s\n", text);
    controller_request_osd(app, text[0] ? text : NULL, 6, tone);
}

static void osd_apply(voice_controller *app, const char *message, double seconds, const char *tone) {
    char text[VOICE_NOTICE_MAX + 1];
    voice_public_text(message, text, sizeof text);
    app->osd_until = controller_now(app) + (seconds > 0 ? seconds : 6);
    free(app->osd_message);
    app->osd_message = text[0] ? strdup(text) : NULL;
    app->has_osd_tone = voice_tone_ok(tone);
    if (app->has_osd_tone) snprintf(app->osd_tone, sizeof app->osd_tone, "%s", tone);
    else app->osd_tone[0] = 0;
}

void controller_request_osd(voice_controller *app, const char *message, double seconds, const char *tone) {
    pthread_mutex_lock(&app->state);
    osd_apply(app, message, seconds, tone);
    pthread_mutex_unlock(&app->state);
}

void controller_report_error(voice_controller *app, const char *error) {
    pthread_mutex_lock(&app->state);
    free(app->error);
    app->error = strdup(error ? error : "Voice command failed");
    if (!controller_recording_active(app)) snprintf(app->phase, sizeof app->phase, "error");
    pthread_mutex_unlock(&app->state);
}

int controller_recording_active(const voice_controller *app) {
    return strcmp(app->phase, "starting") == 0 || strcmp(app->phase, "recording") == 0
        || strcmp(app->phase, "stopping") == 0 || strcmp(app->phase, "transcribing") == 0;
}

void controller_expire_retry(voice_controller *app, int force) {
    double now = controller_now(app);
    if (app->has_active_retry && (force || now > app->active_retry_deadline)) {
        if (app->active_retry_op) atomic_store(&app->active_retry_op->cancelled, 1);
        if (app->active_retry_path) unlink(app->active_retry_path);
        free(app->active_retry_path);
        app->active_retry_path = NULL;
        if (!force && app->record_op == app->active_retry_op) {
            if (app->input_op) atomic_store(&app->input_op->cancelled, 1);
            free(app->error);
            app->error = strdup("Recording retry expired; record again");
            snprintf(app->phase, sizeof app->phase, "error");
        }
        controller_op_release(app->active_retry_op);
        app->active_retry_op = NULL;
        app->has_active_retry = 0;
    }
    if (app->has_retry && (force || now > app->retry_deadline)) {
        if (app->retry_path) unlink(app->retry_path);
        free(app->retry_path);
        app->retry_path = NULL;
        controller_snap_release(app->retry_target);
        app->retry_target = NULL;
        free(app->retry_previous);
        app->retry_previous = NULL;
        app->has_retry = 0;
        if (app->retry_lease && app->deps.engines.release) app->deps.engines.release(app->retry_lease);
        app->retry_lease = NULL;
    }
}

static void *stop_held(voice_controller *app, int discard, int target_lost) {
    if (!target_lost) {
        app->recovery_revision++;
        if (app->record_op) app->record_op->voice_target_lost = 0;
    }
    if (app->input_op) atomic_store(&app->input_op->cancelled, 1);
    if (app->speak_op) atomic_store(&app->speak_op->cancelled, 1);
    if (app->record_op) atomic_store(&app->record_op->cancelled, 1);
    void *owner = controller_capture_unpublish(app);
    app->has_record_started = 0;
    if (discard) {
        free(app->pending);
        app->pending = NULL;
        app->send_when_idle = 0;
    }
    controller_expire_retry(app, 1);
    snprintf(app->phase, sizeof app->phase, "%s", (app->draft || app->pending) ? "draft" : "idle");
    free(app->error);
    app->error = NULL;
    app->audible = 0;
    return owner;
}

static void finish_capture(voice_controller *app, void *owner) {
    controller_capture_release(owner);
    if (app->deps.audio.stop) app->deps.audio.stop(app->deps.audio.user);
}

void controller_stop(voice_controller *app, int discard, int target_lost) {
    pthread_mutex_lock(&app->state);
    void *owner = stop_held(app, discard, target_lost);
    pthread_mutex_unlock(&app->state);
    finish_capture(app, owner);
}

static int turn_has(char turns[][CTRL_ID], size_t count, const char *turn) {
    if (!turn) return 0;
    for (size_t i = 0; i < count; i++) if (strcmp(turns[i], turn) == 0) return 1;
    return 0;
}

static int turn_add(char turns[][CTRL_ID], size_t *count, const char *turn) {
    if (!turn || turn_has(turns, *count, turn) || *count >= CTRL_MAX_TURNS) return 0;
    snprintf(turns[(*count)++], CTRL_ID, "%s", turn);
    return 1;
}

static char *dispatch_status(voice_controller *app) {
    char *status = controller_status_json(app);
    char *env = voice_envelope_ok(status ? status : "{}");
    free(status);
    return env;
}

static char *fail(voice_controller *app, const char *message) {
    controller_report_error(app, message);
    controller_notice(app, "Voice unavailable", message, "red");
    return voice_envelope_error(message);
}

static int json_bool(yyjson_val *value, int fallback) {
    if (yyjson_is_bool(value)) return yyjson_get_bool(value);
    return fallback;
}

static void copy_token(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

char *controller_dispatch(voice_controller *app, const char *request_json) {
    if (!app) return voice_envelope_error("Voice controller is unavailable");
    if (!request_json) return fail(app, "Voice request is empty");
    if (strlen(request_json) > VOICE_REQUEST_MAX) return fail(app, "Voice request exceeds the size limit");
    yyjson_doc *doc = yyjson_read(request_json, strlen(request_json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return fail(app, "Voice request is not valid JSON");
    }
    yyjson_val *action_val = yyjson_obj_get(root, "action");
    if (!yyjson_is_str(action_val)) {
        yyjson_doc_free(doc);
        return fail(app, "Unknown voice command");
    }
    const char *action = yyjson_get_str(action_val);
    char err[CTRL_MSG] = {0};
    if (strncmp(action, "fixture-", 8) == 0) {
        yyjson_doc_free(doc);
        return fail(app, "Unknown voice command");
    }
    if (strncmp(action, "select:", 7) == 0) {
        pthread_mutex_lock(&app->state);
        session_entry *entry = controller_find(app, action + 7);
        if (!entry) {
            pthread_mutex_unlock(&app->state);
            yyjson_doc_free(doc);
            return fail(app, "That voice session is no longer available");
        }
        target_data copy = entry->target->data;
        int managed = copy.managed;
        pthread_mutex_unlock(&app->state);
        if (managed) {
            target_data validated;
            if (controller_validate_attachment(app, &copy, &validated, err, sizeof err) != 0
                || !controller_same_target(&validated, &copy)) {
                yyjson_doc_free(doc);
                return fail(app, err[0] ? err : "That Pi voice session changed");
            }
        }
        pthread_mutex_lock(&app->state);
        if (controller_find(app, action + 7) != entry) {
            pthread_mutex_unlock(&app->state);
            yyjson_doc_free(doc);
            return fail(app, "That voice session is no longer available");
        }
        controller_remember(app);
        if (app->has_token && strcmp(app->token, action + 7) != 0 && app->target
            && (strcmp(app->target->data.harness, "pi") == 0 || strcmp(app->target->data.harness, "qwen-pi") == 0)) {
            if (controller_retain(app, app->pending, app->token, &app->target->data, NULL)) {
                session_entry *old = controller_find(app, app->token);
                if (old) {
                    free(old->pending);
                    old->pending = NULL;
                }
                free(app->pending);
                app->pending = NULL;
            }
            if (controller_recording_active(app) && app->record_op) app->record_op->voice_target_lost = 1;
        }
        pthread_mutex_unlock(&app->state);
        controller_stop(app, 1, 1);
        pthread_mutex_lock(&app->state);
        app->selection_initialized = 1;
        app->has_reconnect = 0;
        controller_load_selected(app, action + 7, 1);
        controller_save_selection(app);
        pthread_mutex_unlock(&app->state);
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strncmp(action, "voice:", 6) == 0) {
        if (!app->deps.audio.set_voice || app->deps.audio.set_voice(app->deps.audio.user, action + 6, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Could not select that voice");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strncmp(action, "speech:", 7) == 0) {
        pthread_mutex_lock(&app->state);
        int live = controller_recording_active(app);
        pthread_mutex_unlock(&app->state);
        if (live) {
            yyjson_doc_free(doc);
            return fail(app, "Finish dictation before changing speech");
        }
        if (app->speak_op) atomic_store(&app->speak_op->cancelled, 1);
        if (app->deps.audio.stop) app->deps.audio.stop(app->deps.audio.user);
        if (!app->deps.audio.set_speech_backend
            || app->deps.audio.set_speech_backend(app->deps.audio.user, action + 7, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Could not select that speech backend");
        }
        pthread_mutex_lock(&app->state);
        int enabled = app->auto_read;
        pthread_mutex_unlock(&app->state);
        if (!app->speech_available && enabled) {
            yyjson_doc_free(doc);
            return fail(app, "Speech is not available on this machine");
        }
        if (app->deps.has_engines && app->deps.engines.has_tts && app->deps.engines.has_tts(app->deps.engines.user)) {
            const char *backend = app->deps.audio.speech_backend ? app->deps.audio.speech_backend(app->deps.audio.user) : "local";
            if (enabled && strcmp(backend, "local") == 0 && app->deps.engines.ensure_resident)
                app->deps.engines.ensure_resident(app->deps.engines.user, "tts");
            else if (app->deps.engines.retire) app->deps.engines.retire(app->deps.engines.user, "tts");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strncmp(action, "stt:", 4) == 0) {
        pthread_mutex_lock(&app->state);
        int live = controller_recording_active(app);
        pthread_mutex_unlock(&app->state);
        if (live) {
            yyjson_doc_free(doc);
            return fail(app, "Finish recording and transcription before changing dictation");
        }
        if (!app->deps.audio.set_stt_backend
            || app->deps.audio.set_stt_backend(app->deps.audio.user, action + 4, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Could not select that dictation backend");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "team-toggle") == 0) {
        pthread_mutex_lock(&app->state);
        app->show_team = !app->show_team;
        controller_save_selection(app);
        pthread_mutex_unlock(&app->state);
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "warm") == 0) {
        pthread_mutex_lock(&app->state);
        controller_population(app);
        pthread_mutex_unlock(&app->state);
        if (app->deps.has_engines && app->deps.engines.warm) app->deps.engines.warm(app->deps.engines.user);
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "attach") == 0) {
        char *attached = controller_attach_json(app, yyjson_obj_get(root, "target"));
        char *env = voice_envelope_fields("attached", attached, 0, 0);
        free(attached);
        yyjson_doc_free(doc);
        return env;
    }
    if (strcmp(action, "detach") == 0) {
        const char *token = yyjson_is_str(yyjson_obj_get(root, "token")) ? yyjson_get_str(yyjson_obj_get(root, "token")) : "";
        int accepted = controller_detach(app, token, root);
        yyjson_doc_free(doc);
        return voice_envelope_fields("accepted", NULL, accepted, 1);
    }
    if (strcmp(action, "recover-copy") == 0 || strcmp(action, "recover-stage") == 0 || strcmp(action, "recover-discard") == 0) {
        int rc = 0;
        if (strcmp(action, "recover-copy") == 0) rc = controller_recover_copy(app, err, sizeof err);
        else if (strcmp(action, "recover-stage") == 0) rc = controller_recover_stage(app, err, sizeof err);
        else rc = controller_recover_discard(app, err, sizeof err);
        yyjson_doc_free(doc);
        if (rc != 0) return fail(app, err[0] ? err : "Could not recover dictation");
        return dispatch_status(app);
    }
    if (strcmp(action, "harness-event") == 0) {
        const char *token = yyjson_is_str(yyjson_obj_get(root, "token")) ? yyjson_get_str(yyjson_obj_get(root, "token")) : "";
        int accepted = controller_harness_event(app, token, yyjson_obj_get(root, "event"));
        yyjson_doc_free(doc);
        return voice_envelope_fields("accepted", NULL, accepted, 1);
    }
    if (strcmp(action, "notify") == 0) {
        const char *token = yyjson_is_str(yyjson_obj_get(root, "token")) ? yyjson_get_str(yyjson_obj_get(root, "token")) : "";
        int accepted = controller_notify(app, token, yyjson_obj_get(root, "event"));
        yyjson_doc_free(doc);
        return voice_envelope_fields("accepted", NULL, accepted, 1);
    }
    if (strcmp(action, "register") == 0) {
        const char *token = yyjson_is_str(yyjson_obj_get(root, "token")) ? yyjson_get_str(yyjson_obj_get(root, "token")) : "";
        if (controller_register(app, token, yyjson_obj_get(root, "target"), err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Could not register the voice session");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "unregister") == 0) {
        const char *token = yyjson_is_str(yyjson_obj_get(root, "token")) ? yyjson_get_str(yyjson_obj_get(root, "token")) : "";
        pthread_mutex_lock(&app->state);
        controller_remove_session(app, token, 1);
        controller_save_selection(app);
        pthread_mutex_unlock(&app->state);
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "notice") == 0) {
        yyjson_val *message = yyjson_obj_get(root, "message");
        if (!yyjson_is_str(message) || !yyjson_get_str(message)[0] || strspn(yyjson_get_str(message), " \t\r\n") == strlen(yyjson_get_str(message))) {
            yyjson_doc_free(doc);
            return fail(app, "Voice notice needs a message");
        }
        const char *tone = yyjson_is_str(yyjson_obj_get(root, "tone")) ? yyjson_get_str(yyjson_obj_get(root, "tone")) : "orange";
        controller_notice(app, yyjson_get_str(message), "", tone);
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "auto") == 0 || strcmp(action, "auto-toggle") == 0) {
        int enabled = strcmp(action, "auto-toggle") == 0 ? !app->auto_read : json_bool(yyjson_obj_get(root, "enabled"), 0);
        if (!app->speech_available) {
            yyjson_doc_free(doc);
            return fail(app, "Speech is not available on this machine");
        }
        pthread_mutex_lock(&app->state);
        app->auto_read = enabled;
        pthread_mutex_unlock(&app->state);
        if (!enabled) {
            if (app->speak_op) atomic_store(&app->speak_op->cancelled, 1);
            if (app->deps.audio.stop) app->deps.audio.stop(app->deps.audio.user);
        }
        if (app->deps.has_engines && app->deps.engines.has_tts && app->deps.engines.has_tts(app->deps.engines.user)) {
            const char *backend = app->deps.audio.speech_backend ? app->deps.audio.speech_backend(app->deps.audio.user) : "local";
            if (enabled && strcmp(backend, "local") == 0 && app->deps.engines.ensure_resident)
                app->deps.engines.ensure_resident(app->deps.engines.user, "tts");
            else if (app->deps.engines.retire) app->deps.engines.retire(app->deps.engines.user, "tts");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "append") == 0 || strcmp(action, "replace") == 0 || strcmp(action, "record") == 0 || strcmp(action, "dictate") == 0) {
        if (strcmp(action, "dictate") == 0) {
            yyjson_val *token = yyjson_obj_get(root, "token");
            if (!yyjson_is_str(token) || !yyjson_get_str(token)[0]) {
                yyjson_doc_free(doc);
                return fail(app, "Voice dictation is not bound to this Pi session");
            }
            if (!app->has_token || strcmp(app->token, yyjson_get_str(token)) != 0) {
                char select[CTRL_TOKEN + 16];
                snprintf(select, sizeof select, "select:%s", yyjson_get_str(token));
                char *selected = controller_dispatch(app, "{\"action\":\"status\"}");
                free(selected);
                char req[CTRL_TOKEN + 32];
                snprintf(req, sizeof req, "{\"action\":\"%s\"}", select);
                char *switched = controller_dispatch(app, req);
                yyjson_doc *check = switched ? yyjson_read(switched, strlen(switched), 0) : NULL;
                int ok = check && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(check), "ok"));
                yyjson_doc_free(check);
                if (!ok) {
                    yyjson_doc_free(doc);
                    return switched ? switched : fail(app, "That voice session is no longer available");
                }
                free(switched);
            }
        }
        const char *mode = strcmp(action, "replace") == 0 ? "replace" : "append";
        if (controller_record(app, mode, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Could not record");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "dictate-cancel") == 0) {
        yyjson_val *token = yyjson_obj_get(root, "token");
        if (yyjson_is_str(token) && yyjson_get_str(token)[0] && app->has_token && strcmp(app->token, yyjson_get_str(token)) != 0) {
            yyjson_doc_free(doc);
            return fail(app, "This Pi session is not recording");
        }
        controller_stop(app, 1, 0);
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "discard") == 0 || strcmp(action, "stop") == 0) {
        controller_stop(app, 1, 0);
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "interact") == 0) {
        if (controller_interact(app, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Voice interaction failed");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "send") == 0) {
        if (controller_send(app, NULL, 0, 0, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Could not send dictation");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "read") == 0) {
        if (controller_read(app, 0, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Could not read the reply");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "retry") == 0) {
        if (controller_retry(app, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "No recording available to retry");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "rebind") == 0) {
        if (controller_rebind(app, err, sizeof err) != 0) {
            yyjson_doc_free(doc);
            return fail(app, err[0] ? err : "Select a voice session first");
        }
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    if (strcmp(action, "status") == 0) {
        yyjson_doc_free(doc);
        return dispatch_status(app);
    }
    yyjson_doc_free(doc);
    (void)copy_token;
    (void)turn_add;
    (void)turn_has;
    return fail(app, "Unknown voice command");
}

void controller_shutdown(voice_controller *app) {
    if (!app) return;
    atomic_store(&app->attachments_closed, 1);
    controller_stop(app, 1, 0);
    pool_shutdown(app);
    if (app->deps.has_engines && app->deps.engines.close) app->deps.engines.close(app->deps.engines.user);
    if (app->deps.has_catalogue && app->deps.catalogue.close) app->deps.catalogue.close(app->deps.catalogue.user);
}

void controller_free(voice_controller *app) {
    if (!app) return;
    if (app->pool_started) controller_shutdown(app);
    pthread_mutex_lock(&app->drain_mu);
    drain_hold *hold = app->drains;
    app->drains = NULL;
    pthread_mutex_unlock(&app->drain_mu);
    while (hold) {
        drain_hold *next = hold->next;
        if (atomic_exchange(&hold->released, 1) == 0 && hold->user) {
            /* Leftover only after audio workers have been joined by the caller. */
        }
        free(hold);
        hold = next;
    }
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) session_clear(&app->sessions[i]);
    controller_snap_release(app->target);
    controller_snap_release(app->retry_target);
    controller_op_release(app->input_op);
    controller_op_release(app->record_op);
    controller_op_release(app->speak_op);
    controller_op_release(app->active_retry_op);
    controller_clear_turns(&app->turns, &app->turn_count, &app->turn_cap);
    free(app->reply);
    free(app->pending);
    free(app->error);
    free(app->recording_label);
    free(app->osd_message);
    free(app->retry_path);
    free(app->retry_previous);
    free(app->active_retry_path);
    if (app->retained) {
        free(app->retained->text);
        free(app->retained);
    }
    registry_free(app->attachments);
    free(app->runtime);
    pthread_mutex_destroy(&app->state);
    pthread_mutex_destroy(&app->delivery);
    pthread_mutex_destroy(&app->drain_mu);
    pthread_mutex_destroy(&app->pool_mu);
    pthread_cond_destroy(&app->pool_cv);
    free(app);
}

typedef struct activity_job {
    voice_controller *app;
    char token[CTRL_TOKEN];
    target_snap *target;
    int revision;
    int draft_revision;
    op_state *operation;
    session_entry *entry;
} activity_job;

static void activity_probe(void *arg) {
    activity_job *job = arg;
    char state[32] = "unknown";
    char draft[32] = {0};
    char err[CTRL_MSG] = {0};
    controller_target_view view;
    controller_view(&job->target->data, &view);
    if (job->app->deps.terminal.activity_snapshot)
        job->app->deps.terminal.activity_snapshot(job->app->deps.terminal.user, &view, state, sizeof state, draft, sizeof draft, err, sizeof err);
    else if (job->app->deps.terminal.activity)
        job->app->deps.terminal.activity(job->app->deps.terminal.user, &view, state, sizeof state, err, sizeof err);
    if (strcmp(state, "done") == 0) snprintf(state, sizeof state, "idle");
    if (strcmp(state, "working") != 0 && strcmp(state, "blocked") != 0 && strcmp(state, "idle") != 0)
        snprintf(state, sizeof state, "unknown");
    pthread_mutex_lock(&job->app->state);
    session_entry *entry = controller_find(job->app, job->token);
    if (entry == job->entry && entry->target && controller_same_target(&entry->target->data, &job->target->data)
        && entry->activity_revision == job->revision) {
        controller_set_activity(entry, state);
        if (job->app->has_token && strcmp(job->app->token, job->token) == 0 && job->app->draft
            && job->operation == job->app->input_op && entry->draft_revision == job->draft_revision
            && !controller_recording_active(job->app)
            && (strcmp(draft, "staged") == 0 || strcmp(draft, "edited") == 0 || strcmp(draft, "empty") == 0 || strcmp(draft, "none") == 0)) {
            snprintf(entry->draft_state, sizeof entry->draft_state, "%s", draft);
            if (strcmp(draft, "staged") != 0 && !job->app->pending) job->app->send_when_idle = 0;
            if (strcmp(draft, "empty") == 0 || strcmp(draft, "none") == 0) {
                job->app->draft = entry->draft = 0;
                if (!job->app->pending) snprintf(job->app->phase, sizeof job->app->phase, "idle");
            }
        }
    }
    if (entry) entry->activity_inflight = 0;
    pthread_mutex_unlock(&job->app->state);
    controller_snap_release(job->target);
    controller_op_release(job->operation);
    free(job);
}

void controller_refresh_activity(voice_controller *app) {
    if (!app->deps.terminal.activity && !app->deps.terminal.activity_snapshot) return;
    pthread_mutex_lock(&app->state);
    session_entry *entry = app->has_token ? controller_find(app, app->token) : NULL;
    if (!entry || entry->activity_inflight) {
        pthread_mutex_unlock(&app->state);
        return;
    }
    double now = controller_now(app);
    if (entry->has_activity_checked && now - entry->activity_checked < VOICE_ACTIVITY_INTERVAL) {
        pthread_mutex_unlock(&app->state);
        return;
    }
    entry->activity_checked = now;
    entry->has_activity_checked = 1;
    entry->activity_inflight = 1;
    activity_job *job = calloc(1, sizeof *job);
    if (!job) {
        entry->activity_inflight = 0;
        pthread_mutex_unlock(&app->state);
        return;
    }
    job->app = app;
    snprintf(job->token, sizeof job->token, "%s", app->token);
    job->target = controller_snap_retain(entry->target);
    job->revision = entry->activity_revision;
    job->draft_revision = entry->draft_revision;
    job->operation = controller_op_retain(app->input_op);
    job->entry = entry;
    pthread_mutex_unlock(&app->state);
    if (controller_submit(app, activity_probe, job) != 0) {
        pthread_mutex_lock(&app->state);
        if (controller_find(app, job->token) == entry) entry->activity_inflight = 0;
        pthread_mutex_unlock(&app->state);
        controller_snap_release(job->target);
        controller_op_release(job->operation);
        free(job);
    }
}

int voice_bind_control(const char *runtime, int *listen_fd, char *err, size_t err_cap) {
    if (listen_fd) *listen_fd = -1;
    if (!runtime || !listen_fd) {
        snprintf(err, err_cap, "Voice runtime is unavailable");
        return -1;
    }
    if (runtime[0] != '/' || strlen(runtime) >= CTRL_PATH - 32 || strstr(runtime, "..")) {
        snprintf(err, err_cap, "Voice runtime path is not private");
        return -1;
    }
    if (mkdir(runtime, 0700) != 0 && errno != EEXIST) {
        snprintf(err, err_cap, "Could not create the voice runtime");
        return -1;
    }
    struct stat st;
    if (lstat(runtime, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
        snprintf(err, err_cap, "Voice runtime is not a private directory");
        return -1;
    }
    char path[CTRL_PATH];
    snprintf(path, sizeof path, "%s/%s", runtime, VOICE_SOCKET_NAME);
    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode) || !S_ISSOCK(st.st_mode)) {
            snprintf(err, err_cap, "Voice control path is not a socket");
            return -1;
        }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int live = 0;
        if (probe >= 0 && strlen(path) < sizeof(struct sockaddr_un) - offsetof(struct sockaddr_un, sun_path)) {
            int flags = fcntl(probe, F_GETFL, 0);
            if (flags >= 0) fcntl(probe, F_SETFL, flags | O_NONBLOCK);
            struct sockaddr_un existing;
            memset(&existing, 0, sizeof existing);
            existing.sun_family = AF_UNIX;
            memcpy(existing.sun_path, path, strlen(path) + 1);
            if (connect(probe, (struct sockaddr *)&existing, sizeof existing) == 0) live = 1;
            else if (errno == EINPROGRESS || errno == EAGAIN) {
                struct pollfd pfd = {.fd = probe, .events = POLLOUT};
                if (poll(&pfd, 1, 200) > 0) {
                    int soerr = 0;
                    socklen_t slen = sizeof soerr;
                    if (getsockopt(probe, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr == 0) live = 1;
                } else live = 1;
            }
        }
        if (probe >= 0) close(probe);
        if (live) {
            snprintf(err, err_cap, "Voice controller is already running");
            return -1;
        }
        unlink(path);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        snprintf(err, err_cap, "Could not create the voice socket");
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        close(fd);
        snprintf(err, err_cap, "Voice control path is too long");
        return -1;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, VOICE_ACCEPT_BACKLOG) != 0) {
        close(fd);
        snprintf(err, err_cap, "Could not bind the voice socket");
        return -1;
    }
    if (chmod(path, 0600) != 0 || lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode) || (st.st_mode & 077) || st.st_uid != getuid()) {
        close(fd);
        unlink(path);
        snprintf(err, err_cap, "Voice control socket is not private");
        return -1;
    }
    *listen_fd = fd;
    return 0;
}
