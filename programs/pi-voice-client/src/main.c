#define _GNU_SOURCE
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "capture.h"
#include "harness.h"
#include "labels.h"
#include "runtime_adapters.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The signal handler and client workers share this flag. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "Signal shutdown requires lock-free atomics");
static atomic_int stop_requested;

static void audible_bridge(void *user) { controller_mark_audible(user); }

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static voice_target *view_target(const controller_target_view *view) {
    voice_target_fields fields = {0};
    fields.socket = view->socket_path;
    fields.pane = view->pane;
    fields.session = view->has_session ? view->session : NULL;
    fields.token = view->token;
    fields.adapter_socket = view->has_adapter ? view->adapter_socket : NULL;
    fields.bridge_id = view->bridge_id;
    fields.harness = view->harness;
    fields.start = view->start;
    fields.pid = view->pid;
    fields.managed = view->managed;
    fields.has_activation = view->has_activation;
    fields.activation = view->activation;
    fields.has_adapter_identity = view->has_adapter;
    fields.adapter_device = view->adapter_device;
    fields.adapter_inode = view->adapter_inode;
    fields.has_socket_identity = view->has_socket_instance;
    fields.socket_device = view->socket_device;
    fields.socket_inode = view->socket_inode;
    return voice_target_new(&fields);
}

static int map_outcome(int rc) {
    if (rc == VOICE_OK) return CTRL_OK;
    if (rc == VOICE_UNCERTAIN) return CTRL_UNCERTAIN;
    if (rc == VOICE_CANCELLED) return CTRL_CANCELLED;
    return CTRL_ERR;
}

static int term_validate_target(void *user, const controller_target_view *view, char *err, size_t cap) {
    voice_target *target = view_target(view);
    yyjson_doc *status = NULL;
    int rc = voice_terminal_validate_target(user, target, &status, err, cap);
    yyjson_doc_free(status);
    voice_target_free(target);
    return map_outcome(rc);
}

static int term_validate(void *user, const controller_target_view *view, char *err, size_t cap) {
    voice_target *target = view_target(view);
    int rc = voice_terminal_validate(user, target, err, cap);
    voice_target_free(target);
    return map_outcome(rc);
}

static int term_activity(void *user, const controller_target_view *view, char *state, size_t cap, char *err, size_t cap_err) {
    voice_target *target = view_target(view);
    int rc = voice_terminal_activity(user, target, state, cap, err, cap_err);
    voice_target_free(target);
    return map_outcome(rc);
}

static int term_snapshot(void *user, const controller_target_view *view, char *agent_status, size_t status_cap, char *draft_state, size_t draft_cap, char *err, size_t err_cap) {
    voice_target *target = view_target(view);
    yyjson_doc *status = NULL;
    int rc = voice_terminal_validate_target(user, target, &status, err, err_cap);
    voice_target_free(target);
    if (rc != VOICE_OK || !status) {
        yyjson_doc_free(status);
        return map_outcome(rc == VOICE_OK ? VOICE_REJECTED : rc);
    }
    yyjson_val *root = yyjson_doc_get_root(status);
    const char *agent = NULL;
    if (yyjson_is_str(yyjson_obj_get(root, "agent_status"))) agent = yyjson_get_str(yyjson_obj_get(root, "agent_status"));
    else if (yyjson_is_str(yyjson_obj_get(root, "state"))) agent = yyjson_get_str(yyjson_obj_get(root, "state"));
    snprintf(agent_status, status_cap, "%s", agent ? agent : "unknown");
    const char *draft = yyjson_is_str(yyjson_obj_get(root, "draft_state")) ? yyjson_get_str(yyjson_obj_get(root, "draft_state")) : "";
    snprintf(draft_state, draft_cap, "%s", draft);
    yyjson_doc_free(status);
    return CTRL_OK;
}

static __thread char speech_backend_buf[32];
static __thread char stt_backend_buf[32];

static const char *audio_speech_backend(void *user) {
    audio_report report;
    memset(&report, 0, sizeof report);
    if (audio_status(user, &report) != 0 || !report.speech_backend[0]) {
        speech_backend_buf[0] = 0;
        return speech_backend_buf;
    }
    snprintf(speech_backend_buf, sizeof speech_backend_buf, "%s", report.speech_backend);
    return speech_backend_buf;
}

static const char *audio_stt_backend(void *user) {
    audio_report report;
    memset(&report, 0, sizeof report);
    if (audio_status(user, &report) != 0 || !report.selected_stt[0]) {
        stt_backend_buf[0] = 0;
        return stt_backend_buf;
    }
    snprintf(stt_backend_buf, sizeof stt_backend_buf, "%s", report.selected_stt);
    return stt_backend_buf;
}

static LabelsCache *label_cache;

static int label_runner(const SocketKey *key, double timeout, char **snapshot_json, void *user) {
    (void)user;
    return labels_run_snapshot(key, timeout, snapshot_json);
}

static double label_clock(void *user) {
    (void)user;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cat_set_active(void *user, const char *const *paths, const uint64_t *devices, const uint64_t *inodes, size_t count) {
    (void)user;
    SocketKey *keys = calloc(count ? count : 1, sizeof *keys);
    if (!keys) return -1;
    for (size_t i = 0; i < count; i++) socket_key_init(&keys[i], paths[i], devices[i], inodes[i]);
    int rc = labels_cache_set_active(label_cache, keys, count);
    for (size_t i = 0; i < count; i++) socket_key_clear(&keys[i]);
    free(keys);
    return rc == LABELS_OK ? 0 : -1;
}

static int cat_refresh(void *user, const char *path, uint64_t device, uint64_t inode) {
    (void)user;
    SocketKey key;
    if (socket_key_init(&key, path, device, inode) != 0) return -1;
    int rc = labels_cache_refresh(label_cache, &key);
    socket_key_clear(&key);
    return rc;
}

static int cat_read_panes(void *user, const char *path, uint64_t device, uint64_t inode, int *authoritative, char ***pane_ids, size_t *count) {
    (void)user;
    *authoritative = 0;
    *pane_ids = NULL;
    *count = 0;
    SocketKey key;
    if (socket_key_init(&key, path, device, inode) != 0) return -1;
    SnapshotState *state = labels_cache_read(label_cache, &key);
    socket_key_clear(&key);
    if (!state) return -1;
    *authoritative = labels_state_authoritative(state, label_clock(NULL));
    size_t n = labels_state_pane_count(state);
    char **ids = calloc(n ? n : 1, sizeof *ids);
    if (!ids) {
        labels_state_free(state);
        return -1;
    }
    for (size_t i = 0; i < n; i++) ids[i] = strdup(labels_state_pane_id(state, i));
    *pane_ids = ids;
    *count = n;
    labels_state_free(state);
    return 0;
}

static SnapshotState *cat_read_snapshot(void *user, const char *path, uint64_t device, uint64_t inode) {
    (void)user;
    SocketKey key;
    if (socket_key_init(&key, path, device, inode) != 0) return NULL;
    SnapshotState *state = labels_cache_read(label_cache, &key);
    socket_key_clear(&key);
    return state;
}

static void cat_close(void *user) {
    (void)user;
    if (label_cache) labels_cache_close(label_cache);
}

static int term_insert(void *user, const controller_target_view *view, const char *text, atomic_int *cancelled, char *err, size_t cap) {
    voice_target *target = view_target(view);
    int rc = voice_terminal_insert_guarded(user, target, text, cancelled, err, cap);
    voice_target_free(target);
    return map_outcome(rc);
}

static int term_submit(void *user, const controller_target_view *view, atomic_int *cancelled, int allow_edited, char *err, size_t cap) {
    voice_target *target = view_target(view);
    int rc = voice_terminal_submit_guarded(user, target, cancelled, allow_edited, err, cap);
    voice_target_free(target);
    return map_outcome(rc);
}

static int herdr_bridge(void *user, const controller_target_view *view, const char *const *argv, char **result_json, char *err, size_t cap) {
    int argc = 0;
    while (argv && argv[argc]) argc++;
    voice_target *target = view_target(view);
    yyjson_doc *result = NULL;
    int rc = voice_herdr_request(user, target, argv, argc, 0, &result, err, cap);
    voice_target_free(target);
    if (rc != VOICE_OK) return CTRL_ERR;
    *result_json = yyjson_val_write(yyjson_doc_get_root(result), 0, NULL);
    yyjson_doc_free(result);
    return *result_json ? CTRL_OK : CTRL_ERR;
}

static int audio_status_json(void *user, char **json) {
    audio_report report;
    if (audio_status(user, &report) != 0) return CTRL_ERR;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "selected_voice", report.selected_voice);
    yyjson_mut_obj_add_strcpy(doc, root, "speech_backend", report.speech_backend);
    yyjson_mut_obj_add_strcpy(doc, root, "selected_stt", report.selected_stt);
    yyjson_mut_val *voices = yyjson_mut_obj(doc);
    for (size_t i = 0; i < report.voice_count; i++) yyjson_mut_obj_add_strcpy(doc, voices, report.voice_ids[i], report.voice_labels[i]);
    yyjson_mut_obj_add_val(doc, root, "voices", voices);
    yyjson_mut_val *speech = yyjson_mut_obj(doc);
    for (size_t i = 0; i < report.speech_backend_count; i++)
        yyjson_mut_obj_add_strcpy(doc, speech, report.speech_backend_ids[i], report.speech_backend_labels[i]);
    yyjson_mut_obj_add_val(doc, root, "speech_backends", speech);
    yyjson_mut_val *stt = yyjson_mut_obj(doc);
    for (size_t i = 0; i < report.stt_count; i++) yyjson_mut_obj_add_strcpy(doc, stt, report.stt_ids[i], report.stt_labels[i]);
    yyjson_mut_obj_add_val(doc, root, "stt_backends", stt);
    yyjson_mut_val *backends = yyjson_mut_obj(doc);
    if (report.has_stt) yyjson_mut_obj_add_strcpy(doc, backends, "stt", report.stt_state);
    if (report.has_tts) yyjson_mut_obj_add_strcpy(doc, backends, "tts", report.tts_state);
    yyjson_mut_obj_add_val(doc, root, "backends", backends);
    yyjson_mut_val *errors = yyjson_mut_obj(doc);
    if (report.has_stt_error) yyjson_mut_obj_add_strcpy(doc, errors, "stt", report.stt_error);
    if (report.has_tts_error) yyjson_mut_obj_add_strcpy(doc, errors, "tts", report.tts_error);
    yyjson_mut_obj_add_val(doc, root, "backend_errors", errors);
    char *microphone = NULL;
    yyjson_doc *microphone_doc = NULL;
    if (audio_microphone_json(user, &microphone) == 0 && microphone)
        microphone_doc = yyjson_read(microphone, strlen(microphone), 0);
    free(microphone);
    yyjson_val *microphone_root = microphone_doc ? yyjson_doc_get_root(microphone_doc) : NULL;
    if (yyjson_is_obj(microphone_root))
        yyjson_mut_obj_add_val(doc, root, "microphone", yyjson_val_mut_copy(doc, microphone_root));
    else yyjson_mut_obj_add_val(doc, root, "microphone", yyjson_mut_obj(doc));
    yyjson_doc_free(microphone_doc);
    *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return CTRL_OK;
}

static int prod_transcribe(void *user, const char *path, atomic_int *cancelled, void (*on_drained)(const char *, void *), void (*release)(void *), void *drain, char *out, size_t out_cap, char *err, size_t err_cap) {
    return audio_transcribe_owned(user, path, cancelled, on_drained, drain, release, out, out_cap, err, err_cap);
}

typedef struct capture_adapter {
    PipeWireCapture *raw;
    PcmChunk *pending;
    size_t count, next;
} capture_adapter;

static PipeWireCapture *cap_raw(controller_capture *cap) {
    return ((capture_adapter *)cap->user)->raw;
}

static controller_capture *wrap_capture(void *raw) {
    controller_capture *cap = calloc(1, sizeof *cap);
    capture_adapter *adapter = calloc(1, sizeof *adapter);
    if (!cap || !adapter) {
        free(cap);
        free(adapter);
        return NULL;
    }
    adapter->raw = raw;
    cap->user = adapter;
    return cap;
}

static int cap_wait_ready(controller_capture *cap, double timeout) {
    return capture_wait_ready(cap_raw(cap), timeout) == 0 ? 0 : -1;
}
static int cap_wait(controller_capture *cap, double timeout, int *code) {
    return capture_wait(cap_raw(cap), timeout, code);
}
static int cap_poll(controller_capture *cap, int *code) {
    int rc = capture_poll(cap_raw(cap), code);
    return rc == 0 ? 1 : 0;
}
static void cap_close(controller_capture *cap) { capture_close(cap_raw(cap)); }
static int cap_drain(controller_capture *cap, int16_t **samples, size_t *count) {
    /* Only the recording worker drains. Preserve the rest of each batch. */
    capture_adapter *adapter = cap->user;
    *samples = NULL;
    *count = 0;
    if (adapter->next == adapter->count) {
        pcm_chunks_free(adapter->pending, adapter->count);
        adapter->pending = NULL;
        adapter->count = adapter->next = 0;
        if (capture_drain_chunks(adapter->raw, &adapter->pending, &adapter->count) != 0) return -1;
    }
    if (!adapter->count) return 1;
    PcmChunk *chunk = &adapter->pending[adapter->next++];
    *samples = chunk->samples;
    *count = chunk->count;
    chunk->samples = NULL;
    return 0;
}
static int cap_progress(controller_capture *cap, double timeout) { return capture_wait_progress(cap_raw(cap), timeout); }
static const char *cap_error(controller_capture *cap) { return capture_error(cap_raw(cap)); }
static double cap_level(controller_capture *cap) { return capture_level(cap_raw(cap)); }
static int cap_clipping(controller_capture *cap) { return capture_clipping(cap_raw(cap)); }
static void cap_destroy(controller_capture *cap) {
    capture_adapter *adapter = cap->user;
    capture_free(adapter->raw);
    pcm_chunks_free(adapter->pending, adapter->count);
    free(adapter);
    free(cap);
}

static controller_capture *prod_start_capture(void *user, const char *path, char *err, size_t cap) {
    void *raw = audio_start_capture(user, path, err, cap);
    if (!raw) return NULL;
    controller_capture *wrapped = wrap_capture(raw);
    if (!wrapped) {
        capture_free(raw);
        snprintf(err, cap, "Could not allocate microphone capture");
        return NULL;
    }
    wrapped->wait_ready = cap_wait_ready;
    wrapped->wait = cap_wait;
    wrapped->poll = cap_poll;
    wrapped->close = cap_close;
    wrapped->drain_chunk = cap_drain;
    wrapped->wait_progress = cap_progress;
    wrapped->error = cap_error;
    wrapped->level = cap_level;
    wrapped->clipping = cap_clipping;
    wrapped->destroy = cap_destroy;
    return wrapped;
}

static int copy_wl(void *user, const char *text, char *err, size_t cap) {
    (void)user;
    ipc_process_request request = {0};
    const char *argv[] = {"wl-copy"};
    request.argv = argv;
    request.argc = 1;
    request.stdin_bytes = (const unsigned char *)text;
    request.stdin_len = strlen(text);
    request.deadline_ms = 5000;
    ipc_process_result result = {0};
    int rc = ipc_process_run(&request, &result, err, cap);
    int failed = rc != 0 || result.exit_code != 0;
    ipc_process_result_free(&result);
    if (failed) snprintf(err, cap, "Could not copy retained dictation; try again");
    return failed ? CTRL_ERR : CTRL_OK;
}

static int extension_installed(char *err, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) {
        snprintf(err, cap, "The Pi voice extension is not installed; run hmswitch first");
        return -1;
    }
    char path[CTRL_PATH];
    snprintf(path, sizeof path, "%s/.pi/agent/extensions/pi-voice.ts", home);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(err, cap, "The Pi voice extension is not installed; run hmswitch first");
        return -1;
    }
    return 0;
}

int voice_launcher_command(const char *harness, int argc, char **argv, char *err, size_t err_cap) {
    if (strcmp(harness, "pi") != 0 && strcmp(harness, "qwen-pi") != 0) {
        snprintf(err, err_cap, "Unsupported voice harness");
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--print") == 0 || strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--mode") == 0
            || strncmp(argv[i], "--mode=", 7) == 0 || strncmp(argv[i], "--print=", 8) == 0) {
            snprintf(err, err_cap, "Voice launchers require an interactive session");
            return -1;
        }
        if (strcmp(harness, "pi") == 0 && (strcmp(argv[i], "--no-extensions") == 0 || strcmp(argv[i], "-ne") == 0)) {
            snprintf(err, err_cap, "The Pi voice launcher requires the installed voice extension");
            return -1;
        }
    }
    return extension_installed(err, err_cap);
}

static int client_call(const char *runtime, const char *request, char **response, int start, char *err, size_t cap) {
    *response = NULL;
    if (start) {
        ipc_process_request svc = {0};
        const char *argv[] = {"systemctl", "--user", "start", "pi-voice.service"};
        svc.argv = argv;
        svc.argc = 4;
        svc.deadline_ms = 15000;
        ipc_process_result result = {0};
        if (ipc_process_run(&svc, &result, err, cap) != 0 || result.exit_code != 0) {
            ipc_process_result_free(&result);
            snprintf(err, cap, "Pi voice service is unavailable");
            return -1;
        }
        ipc_process_result_free(&result);
    }
    char path[CTRL_PATH];
    snprintf(path, sizeof path, "%s/%s", runtime, VOICE_SOCKET_NAME);
    yyjson_doc *doc = yyjson_read(request, strlen(request), 0);
    if (!doc) return -1;
    for (int attempt = 0; attempt < 40; attempt++) {
        ipc_unix_request call = {0};
        call.socket_path = path;
        call.request = doc;
        call.deadline_ms = 15000;
        int sent = 0;
        yyjson_doc *incoming = NULL;
        int rc = ipc_unix_json(&call, &sent, &incoming, err, cap);
        if (rc == IPC_OK && incoming) {
            yyjson_val *root = yyjson_doc_get_root(incoming);
            if (!yyjson_is_true(yyjson_obj_get(root, "ok"))) {
                const char *message = yyjson_get_str(yyjson_obj_get(root, "error"));
                snprintf(err, cap, "%s", message ? message : "Voice command failed");
                yyjson_doc_free(incoming);
                yyjson_doc_free(doc);
                return -1;
            }
            *response = yyjson_val_write(root, 0, NULL);
            yyjson_doc_free(incoming);
            yyjson_doc_free(doc);
            return 0;
        }
        yyjson_doc_free(incoming);
        if (sent) {
            yyjson_doc_free(doc);
            snprintf(err, cap, "Voice command outcome is unknown; check status before trying again");
            return -1;
        }
        if (!start || attempt == 39) break;
        usleep(50000);
    }
    yyjson_doc_free(doc);
    snprintf(err, cap, "Pi voice service is unavailable");
    return -1;
}

typedef struct client_job {
    int fd;
    voice_controller *app;
} client_job;

typedef struct client_pool {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    client_job jobs[VOICE_ACCEPT_BACKLOG];
    int count;
    int stop;
    pthread_t threads[VOICE_HANDLER_THREADS];
    int nthreads;
} client_pool;

static int write_frame(int fd, const char *text) {
    size_t left = text ? strlen(text) : 0;
    const char *cursor = text ? text : "";
    while (left) {
        ssize_t wrote = write(fd, cursor, left);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += wrote;
        left -= (size_t)wrote;
    }
    while (write(fd, "\n", 1) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static void handle_client(voice_controller *app, int client) {
    struct ucred cred;
    socklen_t cred_len = sizeof cred;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 || cred.uid != getuid()) return;
    char *buf = malloc(VOICE_REQUEST_MAX + 1);
    if (!buf) return;
    size_t n = 0;
    int complete = 0;
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!stop_requested && n < VOICE_REQUEST_MAX) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - started.tv_sec) + (double)(now.tv_nsec - started.tv_nsec) / 1e9;
        if (elapsed >= 5.0) break;
        int wait_ms = (int)((5.0 - elapsed) * 1000);
        if (wait_ms > 200) wait_ms = 200;
        if (wait_ms < 1) wait_ms = 1;
        struct pollfd pfd = {.fd = client, .events = POLLIN};
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        ssize_t got = read(client, buf + n, VOICE_REQUEST_MAX - n);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        n += (size_t)got;
        if (memchr(buf, '\n', n)) {
            complete = 1;
            break;
        }
    }
    if (stop_requested) {
        free(buf);
        return;
    }
    buf[n] = 0;
    char *nl = memchr(buf, '\n', n);
    char *response;
    if (!complete || !nl) response = voice_envelope_error("Voice request exceeds the size limit");
    else {
        *nl = 0;
        response = controller_dispatch(app, buf);
    }
    if (response) write_frame(client, response);
    free(response);
    free(buf);
}

static void *client_main(void *arg) {
    client_pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mu);
        while (!pool->count && !pool->stop && !stop_requested) pthread_cond_wait(&pool->cv, &pool->mu);
        if (!pool->count && (pool->stop || stop_requested)) {
            pthread_mutex_unlock(&pool->mu);
            return NULL;
        }
        client_job job = pool->jobs[0];
        memmove(pool->jobs, pool->jobs + 1, (size_t)(pool->count - 1) * sizeof pool->jobs[0]);
        pool->count--;
        pthread_cond_signal(&pool->cv);
        pthread_mutex_unlock(&pool->mu);
        handle_client(job.app, job.fd);
        close(job.fd);
    }
}

static int serve_loop(voice_controller *app, int fd) {
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    client_pool pool;
    memset(&pool, 0, sizeof pool);
    pthread_mutex_init(&pool.mu, NULL);
    pthread_cond_init(&pool.cv, NULL);
    for (int i = 0; i < VOICE_HANDLER_THREADS; i++) {
        if (pthread_create(&pool.threads[i], NULL, client_main, &pool) != 0) break;
        pool.nthreads++;
    }
    double next_monitor = controller_now(app);
    while (!stop_requested) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        poll(&pfd, 1, 200);
        if (controller_now(app) >= next_monitor) {
            controller_monitor(app);
            next_monitor = controller_now(app) + VOICE_MONITOR_SECONDS;
        }
        if (!(pfd.revents & POLLIN)) continue;
        int client = accept(fd, NULL, NULL);
        if (client < 0) continue;
        pthread_mutex_lock(&pool.mu);
        if (stop_requested || pool.count == VOICE_ACCEPT_BACKLOG) {
            pthread_mutex_unlock(&pool.mu);
            close(client);
            continue;
        }
        pool.jobs[pool.count].fd = client;
        pool.jobs[pool.count].app = app;
        pool.count++;
        pthread_cond_signal(&pool.cv);
        pthread_mutex_unlock(&pool.mu);
    }
    pthread_mutex_lock(&pool.mu);
    pool.stop = 1;
    pthread_cond_broadcast(&pool.cv);
    pthread_mutex_unlock(&pool.mu);
    for (int i = 0; i < pool.nthreads; i++) pthread_join(pool.threads[i], NULL);
    pthread_cond_destroy(&pool.cv);
    pthread_mutex_destroy(&pool.mu);
    return 0;
}

static int cmd_serve(void) {
    char runtime[CTRL_PATH];
    char err[CTRL_MSG];
    if (voice_runtime_dir_resolve(runtime, sizeof runtime, err, sizeof err) != 0) {
        fprintf(stderr, "pi-voice: %s\n", err);
        return 1;
    }
    const char *config_path = getenv("PI_VOICE_CONFIG");
    char fallback[CTRL_PATH];
    if (!config_path) {
        const char *home = getenv("HOME");
        snprintf(fallback, sizeof fallback, "%s/.config/pi-voice/config.json", home ? home : "");
        config_path = fallback;
    }
    yyjson_doc *config = yyjson_read_file(config_path, 0, NULL, NULL);
    if (!config) {
        fprintf(stderr, "pi-voice: could not read voice configuration\n");
        return 1;
    }
    voice_runtime *rt = voice_runtime_open(runtime, config, NULL, err, sizeof err);
    yyjson_doc_free(config);
    if (!rt) {
        fprintf(stderr, "pi-voice: %s\n", err[0] ? err : "could not open voice runtime");
        return 1;
    }
    controller_deps deps = {0};
    deps.terminal.user = voice_runtime_terminal(rt);
    deps.terminal.validate_target = term_validate_target;
    deps.terminal.validate = term_validate;
    deps.terminal.activity = term_activity;
    deps.terminal.activity_snapshot = term_snapshot;
    deps.terminal.insert_guarded = term_insert;
    deps.terminal.submit_guarded = term_submit;
    deps.audio.user = voice_runtime_audio(rt);
    deps.audio.status_json = audio_status_json;
    deps.audio.stop = (void (*)(void *))audio_stop;
    deps.audio.cue = (int (*)(void *, int, char *, size_t))audio_cue;
    deps.audio.start_capture = prod_start_capture;
    deps.audio.transcribe_owned = prod_transcribe;
    deps.audio.speak = (int (*)(void *, const char *, atomic_int *, char *, size_t))audio_speak;
    deps.audio.set_voice = (int (*)(void *, const char *, char *, size_t))audio_set_voice;
    deps.audio.set_speech_backend = (int (*)(void *, const char *, char *, size_t))audio_set_speech_backend;
    deps.audio.set_stt_backend = (int (*)(void *, const char *, char *, size_t))audio_set_stt_backend;
    deps.audio.speech_backend = audio_speech_backend;
    deps.audio.stt_backend = audio_stt_backend;
    deps.audio.playing = (int (*)(void *))audio_playing;
    deps.herdr.user = voice_terminal_herdr(voice_runtime_terminal(rt));
    deps.herdr.request = herdr_bridge;
    LabelsCacheConfig labels_cfg = {0};
    labels_cfg.runner = label_runner;
    labels_cfg.clock = label_clock;
    labels_cfg.max_keys = LABELS_MAX_KEYS;
    label_cache = labels_cache_new(&labels_cfg);
    if (label_cache) {
        deps.has_catalogue = 1;
        deps.catalogue.user = label_cache;
        deps.catalogue.max_keys = LABELS_MAX_KEYS;
        deps.catalogue.set_active = cat_set_active;
        deps.catalogue.refresh = cat_refresh;
        deps.catalogue.read_panes = cat_read_panes;
        deps.catalogue.read_snapshot = cat_read_snapshot;
        deps.catalogue.close = cat_close;
    }
    audio_report backends;
    memset(&backends, 0, sizeof backends);
    deps.speech_available = audio_status(voice_runtime_audio(rt), &backends) == 0 && backends.speech_backend_count > 0;
    deps.auto_speak = voice_runtime_auto_speak(rt);
    deps.copy_text = copy_wl;
    voice_controller *app = controller_create(voice_runtime_path(rt), &deps);
    if (!app) {
        labels_cache_free(label_cache);
        label_cache = NULL;
        voice_runtime_close(rt);
        return 1;
    }
    audio_set_audible(voice_runtime_audio(rt), audible_bridge, app);
    controller_restore(app);
    int fd = -1;
    if (voice_bind_control(voice_runtime_path(rt), &fd, err, sizeof err) != 0) {
        fprintf(stderr, "pi-voice: %s\n", err);
        controller_shutdown(app);
        audio_stop(voice_runtime_audio(rt));
        audio_drain(voice_runtime_audio(rt));
        controller_free(app);
        labels_cache_free(label_cache);
        label_cache = NULL;
        voice_runtime_close(rt);
        return 1;
    }
    serve_loop(app, fd);
    close(fd);
    char sock[CTRL_PATH];
    snprintf(sock, sizeof sock, "%s/%s", voice_runtime_path(rt), VOICE_SOCKET_NAME);
    unlink(sock);
    /* Keep controller callbacks alive through audio_drain. voice_runtime_close
     * then unbinds microphone adapters before freeing audio. */
    controller_shutdown(app);
    audio_stop(voice_runtime_audio(rt));
    audio_drain(voice_runtime_audio(rt));
    controller_free(app);
    labels_cache_free(label_cache);
    label_cache = NULL;
    voice_runtime_close(rt);
    return 0;
}

static int launch(int argc, char **argv, const char *harness) {
    char err[CTRL_MSG];
    if (strcmp(getenv("HERDR_ENV") ? getenv("HERDR_ENV") : "", "1") != 0 || !getenv("HERDR_PANE_ID")) {
        fprintf(stderr, "pi-voice: Run the voice launcher inside the Herdr pane you want to use\n");
        return 1;
    }
    if (strcmp(getenv("PI_TEAM_CHILD") ? getenv("PI_TEAM_CHILD") : "", "1") == 0) {
        fprintf(stderr, "pi-voice: Team panes are not voice sessions\n");
        return 1;
    }
    if (voice_launcher_command(harness, argc, argv, err, sizeof err) != 0) {
        fprintf(stderr, "pi-voice: %s\n", err);
        return 1;
    }
    char runtime[CTRL_PATH];
    if (voice_runtime_dir_resolve(runtime, sizeof runtime, err, sizeof err) != 0) {
        fprintf(stderr, "pi-voice: %s\n", err);
        return 1;
    }
    char token[33];
    FILE *urandom = fopen("/dev/urandom", "rb");
    unsigned char bytes[16];
    if (!urandom || fread(bytes, 1, 16, urandom) != 16) {
        if (urandom) fclose(urandom);
        return 1;
    }
    fclose(urandom);
    for (int i = 0; i < 16; i++) sprintf(token + i * 2, "%02x", bytes[i]);
    char directory[CTRL_PATH];
    if (strlen(runtime) + 1 + strlen(token) >= sizeof directory) return 1;
    memcpy(directory, runtime, strlen(runtime));
    directory[strlen(runtime)] = '/';
    memcpy(directory + strlen(runtime) + 1, token, strlen(token) + 1);
    if (mkdir(directory, 0700) != 0) return 1;
    char *response = NULL;
    if (client_call(runtime, "{\"action\":\"status\"}", &response, 1, err, sizeof err) != 0) {
        fprintf(stderr, "pi-voice: %s\n", err);
        rmdir(directory);
        return 1;
    }
    free(response);
    response = NULL;
    pid_t child = fork();
    if (child < 0) {
        rmdir(directory);
        return 1;
    }
    if (child == 0) {
        setenv("AGENT_VOICE_TOKEN", token, 1);
        setenv("AGENT_VOICE_KIND", harness, 1);
        char sock[CTRL_PATH], adapter[CTRL_PATH], pidbuf[32];
        if (strlen(runtime) + strlen(VOICE_SOCKET_NAME) + 1 >= sizeof sock) _exit(127);
        memcpy(sock, runtime, strlen(runtime));
        sock[strlen(runtime)] = '/';
        memcpy(sock + strlen(runtime) + 1, VOICE_SOCKET_NAME, strlen(VOICE_SOCKET_NAME) + 1);
        if (strlen(directory) + 8 >= sizeof adapter) _exit(127);
        memcpy(adapter, directory, strlen(directory));
        memcpy(adapter + strlen(directory), "/pi.sock", 8);
        snprintf(pidbuf, sizeof pidbuf, "%d", getppid());
        setenv("AGENT_VOICE_SOCKET", sock, 1);
        setenv("AGENT_VOICE_ADAPTER_SOCKET", adapter, 1);
        setenv("AGENT_VOICE_LAUNCH_PID", pidbuf, 1);
        char **args = calloc((size_t)argc + 2, sizeof *args);
        args[0] = (char *)harness;
        for (int i = 0; i < argc; i++) args[i + 1] = argv[i];
        execvp(harness, args);
        _exit(127);
    }
    signal(SIGINT, SIG_IGN);
    char start[CTRL_START] = {0};
    for (int i = 0; i < 50 && process_start_ticks(child, start, sizeof start) != 0; i++) usleep(20000);
    int status = 1;
    char adapter[CTRL_PATH];
    if (strlen(directory) + 8 >= sizeof adapter) return 1;
    memcpy(adapter, directory, strlen(directory));
    memcpy(adapter + strlen(directory), "/pi.sock", 8);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "action", "register");
    yyjson_mut_obj_add_strcpy(doc, root, "token", token);
    yyjson_mut_val *target = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, target, "pane", getenv("HERDR_PANE_ID"));
    yyjson_mut_obj_add_strcpy(doc, target, "socket", getenv("HERDR_SOCKET_PATH") ? getenv("HERDR_SOCKET_PATH") : "");
    yyjson_mut_obj_add_int(doc, target, "pid", child);
    yyjson_mut_obj_add_strcpy(doc, target, "start", start);
    yyjson_mut_obj_add_strcpy(doc, target, "harness", harness);
    yyjson_mut_obj_add_strcpy(doc, target, "adapter_socket", adapter);
    yyjson_mut_obj_add_val(doc, root, "target", target);
    char *request = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    free(response);
    response = NULL;
    int registered = client_call(runtime, request, &response, 1, err, sizeof err) == 0;
    free(request);
    free(response);
    if (!registered) {
        fprintf(stderr, "pi-voice: %s\n", err);
        kill(child, SIGTERM);
        int reaped = 0;
        for (int i = 0; i < 100; i++) {
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child || (result < 0 && errno == ECHILD)) {
                reaped = 1;
                break;
            }
            usleep(100000);
        }
        if (!reaped) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
    } else {
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    char unregister[256];
    snprintf(unregister, sizeof unregister, "{\"action\":\"unregister\",\"token\":\"%s\"}", token);
    response = NULL;
    client_call(runtime, unregister, &response, 0, err, sizeof err);
    free(response);
    unlink(adapter);
    rmdir(directory);
    if (registered && WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

int voice_main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "serve") == 0) return cmd_serve();
    static const char *commands[] = {
        "interact", "record", "send", "read", "stop", "status", "auto", "voice", "retry", "rebind",
        "discard", "append", "replace", "recover-copy", "recover-stage", "recover-discard", NULL
    };
    if (argc > 1) {
        int known = 0;
        for (int i = 0; commands[i]; i++) if (strcmp(argv[1], commands[i]) == 0) known = 1;
        if (known) {
            char err[CTRL_MSG];
            char runtime[CTRL_PATH];
            if (voice_runtime_dir_resolve(runtime, sizeof runtime, err, sizeof err) != 0) {
                fprintf(stderr, "pi-voice: %s\n", err);
                return 1;
            }
            char request[512];
            if (strcmp(argv[1], "auto") == 0) {
                if (argc != 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0)) {
                    fprintf(stderr, "pi-voice: Usage: pi-voice auto on|off\n");
                    return 1;
                }
                snprintf(request, sizeof request, "{\"action\":\"auto\",\"enabled\":%s}", strcmp(argv[2], "on") == 0 ? "true" : "false");
            } else if (strcmp(argv[1], "voice") == 0) {
                if (argc != 3) {
                    fprintf(stderr, "pi-voice: Usage: pi-voice voice CHARACTER\n");
                    return 1;
                }
                snprintf(request, sizeof request, "{\"action\":\"voice:%s\"}", argv[2]);
            } else snprintf(request, sizeof request, "{\"action\":\"%s\"}", argv[1]);
            char *response = NULL;
            if (client_call(runtime, request, &response, 1, err, sizeof err) != 0) {
                fprintf(stderr, "pi-voice: %s\n", err);
                return 1;
            }
            yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
            if (!doc || !yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok"))) {
                const char *message = doc && yyjson_is_str(yyjson_obj_get(yyjson_doc_get_root(doc), "error"))
                    ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "error")) : "Voice command failed";
                fprintf(stderr, "pi-voice: %s\n", message);
                yyjson_doc_free(doc);
                free(response);
                return 1;
            }
            if (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "auto") == 0 || strcmp(argv[1], "voice") == 0) {
                yyjson_mut_doc *mut = yyjson_doc_mut_copy(doc, NULL);
                yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(mut), "reply");
                char *printed = yyjson_mut_write(mut, YYJSON_WRITE_PRETTY, NULL);
                if (printed) printf("%s\n", printed);
                free(printed);
                yyjson_mut_doc_free(mut);
            }
            yyjson_doc_free(doc);
            free(response);
            return 0;
        }
    }
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("Usage: <pi|qwen-pi>-voice [options]\n"
               "       pi-voice interact|record|send|read|stop|status\n"
               "       pi-voice retry|rebind|discard|append|replace\n"
               "       pi-voice recover-copy|recover-stage|recover-discard\n"
               "       pi-voice auto on|off\n"
               "       pi-voice voice CHARACTER\n\n"
               "Super+Space: show the top dictation card and record, stop, or send the selected Pi session. "
               "Super+Shift+Space: send. Super+R: read/stop.\n"
               "Run inside Herdr. Prefix conflicting options with --.\n");
        return 0;
    }
    int offset = argc > 1 && strcmp(argv[1], "--") == 0 ? 2 : 1;
    const char *harness = getenv("AGENT_VOICE_LAUNCH_KIND");
    if (!harness || !harness[0]) harness = "pi";
    return launch(argc - offset, argv + offset, harness);
}

int main(int argc, char **argv) {
    return voice_main(argc, argv);
}
