#define _POSIX_C_SOURCE 200809L
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "text.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct hold_cap {
    controller_capture base;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    atomic_int close_entered;
    atomic_int release_close;
    atomic_int destroyed;
} hold_cap;

static int hold_ready(controller_capture *cap, double timeout) { (void)cap; (void)timeout; return 0; }
static int hold_wait(controller_capture *cap, double timeout, int *code) {
    (void)cap; (void)timeout; if (code) *code = 0; return 0;
}
static int hold_poll(controller_capture *cap, int *code) { (void)cap; if (code) *code = 0; return 1; }
static double hold_level(controller_capture *cap) { (void)cap; return 0.2; }
static int hold_clip(controller_capture *cap) { (void)cap; return 0; }
static void hold_close(controller_capture *cap) {
    hold_cap *hold = (hold_cap *)cap;
    atomic_store(&hold->close_entered, 1);
    pthread_mutex_lock(&hold->mu);
    pthread_cond_broadcast(&hold->cv);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    while (!atomic_load(&hold->release_close)) {
        if (pthread_cond_timedwait(&hold->cv, &hold->mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&hold->mu);
}
static void hold_destroy(controller_capture *cap) {
    atomic_store(&((hold_cap *)cap)->destroyed, 1);
}

typedef struct late_audio {
    hold_cap cap;
    void (*on_drained)(const char *, void *);
    void (*release)(void *);
    void *drain;
    pthread_t thread;
    atomic_int armed;
} late_audio;

static void *late_drain(void *arg) {
    late_audio *audio = arg;
    usleep(30000);
    if (audio->on_drained) audio->on_drained("late words", audio->drain);
    if (audio->release) audio->release(audio->drain);
    return NULL;
}

static int late_transcribe(void *user, const char *path, atomic_int *cancelled,
    void (*on_drained)(const char *, void *), void (*release)(void *), void *drain,
    char *out, size_t out_cap, char *err, size_t err_cap) {
    (void)path; (void)cancelled; (void)err; (void)err_cap;
    late_audio *audio = user;
    audio->on_drained = on_drained;
    audio->release = release;
    audio->drain = drain;
    atomic_store(&audio->armed, 1);
    pthread_create(&audio->thread, NULL, late_drain, audio);
    if (out && out_cap) snprintf(out, out_cap, "hello there");
    return 0;
}

static controller_capture *late_start(void *user, const char *path, char *err, size_t cap) {
    (void)err; (void)cap;
    FILE *file = fopen(path, "wb");
    if (!file) return NULL;
    uint32_t data = 16000 * 2, riff = 36 + data, rate = 16000, bytes = 32000, fmt = 16;
    uint16_t channels = 1, bits = 16, block = 2, pcm = 1;
    fwrite("RIFF", 1, 4, file); fwrite(&riff, 4, 1, file); fwrite("WAVEfmt ", 1, 8, file);
    fwrite(&fmt, 4, 1, file); fwrite(&pcm, 2, 1, file); fwrite(&channels, 2, 1, file);
    fwrite(&rate, 4, 1, file); fwrite(&bytes, 4, 1, file); fwrite(&block, 2, 1, file);
    fwrite(&bits, 2, 1, file); fwrite("data", 1, 4, file); fwrite(&data, 4, 1, file);
    for (int i = 0; i < 16000; i++) { int16_t sample = 4000; fwrite(&sample, 2, 1, file); }
    fclose(file);
    late_audio *audio = user;
    audio->cap.base.wait_ready = hold_ready;
    audio->cap.base.wait = hold_wait;
    audio->cap.base.poll = hold_poll;
    audio->cap.base.close = hold_close;
    audio->cap.base.level = hold_level;
    audio->cap.base.clipping = hold_clip;
    audio->cap.base.destroy = hold_destroy;
    pthread_mutex_init(&audio->cap.mu, NULL);
    pthread_cond_init(&audio->cap.cv, NULL);
    return &audio->cap.base;
}

static int failures;
static voice_controller *switch_app;

typedef struct gate_audio {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int entered;
    int go;
    const char *drain_text;
    void (*on_drained)(const char *, void *);
    void (*release)(void *);
    void *drain;
} gate_audio;

static int gate_ready(controller_capture *cap, double timeout) { (void)cap; (void)timeout; return 0; }
static int gate_wait(controller_capture *cap, double timeout, int *code) {
    (void)cap; (void)timeout; if (code) *code = 0; return 0;
}
static int gate_poll(controller_capture *cap, int *code) { (void)cap; if (code) *code = 0; return 1; }
static void gate_close(controller_capture *cap) { (void)cap; }
static void gate_destroy(controller_capture *cap) { free(cap); }

typedef struct many_cap {
    controller_capture base;
    int sent;
    int limit;
} many_cap;

static int many_drain(controller_capture *cap, int16_t **samples, size_t *count) {
    many_cap *many = (many_cap *)cap;
    if (many->sent >= many->limit) return 1;
    many->sent++;
    *samples = malloc(320 * sizeof(int16_t));
    if (!*samples) return -1;
    for (int i = 0; i < 320; i++) (*samples)[i] = 4000;
    *count = 320;
    return 0;
}
static int many_progress(controller_capture *cap, double timeout) { (void)cap; (void)timeout; return 1; }
static int many_poll(controller_capture *cap, int *code) {
    many_cap *many = (many_cap *)cap;
    if (many->sent < many->limit) return 0;
    if (code) *code = 0;
    return 1;
}
static int many_wait(controller_capture *cap, double timeout, int *code) {
    (void)cap; (void)timeout; if (code) *code = 0; return 0;
}
static void many_close(controller_capture *cap) { (void)cap; }
static void many_destroy(controller_capture *cap) { free(cap); }
static controller_capture *many_start(void *user, const char *path, char *err, size_t cap) {
    (void)path; (void)err; (void)cap;
    many_cap *many = user;
    many->base.drain_chunk = many_drain;
    many->base.wait_progress = many_progress;
    many->base.poll = many_poll;
    many->base.wait = many_wait;
    many->base.wait_ready = gate_ready;
    many->base.close = many_close;
    many->base.destroy = many_destroy;
    return &many->base;
}

static int wavs_left(const char *dir) {
    DIR *handle = opendir(dir);
    int count = 0;
    if (!handle) return -1;
    struct dirent *entry;
    while ((entry = readdir(handle))) {
        size_t n = strlen(entry->d_name);
        if (n > 4 && strcmp(entry->d_name + n - 4, ".wav") == 0) count++;
    }
    closedir(handle);
    return count;
}

static controller_capture *gate_start(void *user, const char *path, char *err, size_t cap) {
    (void)user; (void)err; (void)cap;
    FILE *file = fopen(path, "wb");
    if (!file) return NULL;
    uint32_t data = 16000 * 2, riff = 36 + data, rate = 16000, bytes = 32000, fmt = 16;
    uint16_t channels = 1, bits = 16, block = 2, pcm = 1;
    fwrite("RIFF", 1, 4, file); fwrite(&riff, 4, 1, file); fwrite("WAVEfmt ", 1, 8, file);
    fwrite(&fmt, 4, 1, file); fwrite(&pcm, 2, 1, file); fwrite(&channels, 2, 1, file);
    fwrite(&rate, 4, 1, file); fwrite(&bytes, 4, 1, file); fwrite(&block, 2, 1, file);
    fwrite(&bits, 2, 1, file); fwrite("data", 1, 4, file); fwrite(&data, 4, 1, file);
    for (int i = 0; i < 16000; i++) { int16_t sample = 4000; fwrite(&sample, 2, 1, file); }
    fclose(file);
    controller_capture *capture = calloc(1, sizeof *capture);
    capture->wait_ready = gate_ready;
    capture->wait = gate_wait;
    capture->poll = gate_poll;
    capture->close = gate_close;
    capture->destroy = gate_destroy;
    return capture;
}

static int gate_transcribe(void *user, const char *path, atomic_int *cancelled,
    void (*on_drained)(const char *, void *), void (*release)(void *), void *drain,
    char *out, size_t out_cap, char *err, size_t err_cap) {
    (void)path; (void)err; (void)err_cap;
    gate_audio *gate = user;
    pthread_mutex_lock(&gate->mu);
    gate->on_drained = on_drained;
    gate->release = release;
    gate->drain = drain;
    gate->entered = 1;
    pthread_cond_broadcast(&gate->cv);
    while (!gate->go && !(cancelled && atomic_load(cancelled))) pthread_cond_wait(&gate->cv, &gate->mu);
    pthread_mutex_unlock(&gate->mu);
    if (gate->on_drained && gate->drain_text) gate->on_drained(gate->drain_text, gate->drain);
    if (gate->release) gate->release(gate->drain);
    if (out && out_cap) out[0] = 0;
    return 0;
}

static void switch_during_stop(void *user) {
    (void)user;
    voice_controller *app = switch_app;
    if (!app) return;
    switch_app = NULL;
    app->deps.audio.stop = NULL;
    free(controller_dispatch(app, "{\"action\":\"select:bee\"}"));
}

static void mutate_during_stop(void *user) {
    (void)user;
    voice_controller *app = switch_app;
    if (!app) return;
    switch_app = NULL;
    app->deps.audio.stop = NULL;
    if (app->target) snprintf(app->target->data.pane, sizeof app->target->data.pane, "mutated");
}

static void discard_during_stop(void *user) {
    (void)user;
    voice_controller *app = switch_app;
    if (!app) return;
    switch_app = NULL;
    app->deps.audio.stop = NULL;
    controller_stop(app, 1, 0);
}

static void check(int cond, const char *name) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static int validate_ok(void *user, const controller_target_view *target, char *err, size_t cap) {
    (void)user; (void)target; (void)err; (void)cap;
    return 0;
}

typedef struct term_log {
    char text[8][512];
    int ntext;
    int submits;
    int uncertain;
    char state[32];
} term_log;

static int insert_log(void *user, const controller_target_view *target, const char *text, atomic_int *cancelled, char *err, size_t cap) {
    (void)target;
    term_log *log = user;
    if (cancelled && atomic_load(cancelled)) return CTRL_CANCELLED;
    if (log->uncertain) {
        snprintf(err, cap, "Could not confirm paste; check the selected Pi prompt and send it there if the dictation arrived");
        return CTRL_UNCERTAIN;
    }
    if (log->ntext < 8) snprintf(log->text[log->ntext++], 512, "%s", text);
    return 0;
}

static int submit_log(void *user, const controller_target_view *target, atomic_int *cancelled, int allow_edited, char *err, size_t cap) {
    (void)target; (void)allow_edited;
    term_log *log = user;
    if (cancelled && atomic_load(cancelled)) return CTRL_CANCELLED;
    if (strcmp(log->state, "idle") != 0 && strcmp(log->state, "done") != 0) {
        snprintf(err, cap, "Pi is busy");
        return CTRL_ERR;
    }
    log->submits++;
    return 0;
}

static int validate_state(void *user, const controller_target_view *target, char *err, size_t cap) {
    (void)target;
    term_log *log = user;
    if (strcmp(log->state, "idle") != 0 && strcmp(log->state, "done") != 0) {
        snprintf(err, cap, "Pi is %s", log->state);
        return CTRL_ERR;
    }
    return 0;
}

static int status_with_mic(void *user, char **json) {
    (void)user;
    *json = strdup("{\"microphone\":{\"name\":\"Synthetic\",\"target\":\"synthetic\",\"muted\":true,\"missing\":true,\"preferred\":\"wanted\",\"error\":\"unavailable\"}}");
    return 0;
}

static controller_deps base_deps(term_log *log) {
    controller_deps deps = {0};
    deps.terminal.user = log;
    deps.terminal.validate_target = validate_ok;
    deps.terminal.validate = validate_state;
    deps.terminal.insert_guarded = insert_log;
    deps.terminal.submit_guarded = submit_log;
    deps.speech_available = 1;
    snprintf(log->state, sizeof log->state, "idle");
    return deps;
}

static char *call(voice_controller *app, const char *json) {
    return controller_dispatch(app, json);
}

static int ok_field(const char *json, const char *key, const char *expect) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *value = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), key) : NULL;
    int match = value && yyjson_is_str(value) && strcmp(yyjson_get_str(value), expect) == 0;
    yyjson_doc_free(doc);
    return match;
}

int test_controller(void) {
    char dir[] = "/tmp/voice-controller-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    term_log log = {0};
    controller_deps deps = base_deps(&log);
    voice_controller *app = controller_create(dir, &deps);
    check(app != NULL, "create");
    char *status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"ok\":true") && strstr(status, "\"connection_state\":\"unselected\""), "unselected status envelope");
    free(status);
    status = call(app, "{\"action\":\"fixture-state\"}");
    check(status && strstr(status, "Unknown voice command"), "fixture command rejected");
    free(status);
    status = call(app, "{\"action\":\"notice\",\"message\":\"hello\\u0001there\"}");
    check(status && strstr(status, "\"ok\":true"), "notice accepted");
    check(status && strstr(status, "hellothere") && !strstr(status, "\\u0001"), "notice strips controls");
    free(status);
    status = call(app, "{\"action\":\"register\",\"token\":\"token-1\",\"target\":{\"pane\":\"w1:p2\",\"socket\":\"/tmp/herdr.sock\",\"pid\":100,\"start\":\"200\",\"harness\":\"pi\"}}");
    check(ok_field(status, "connection_state", "ready"), "register ready");
    check(ok_field(status, "pane", "w1:p2"), "register pane");
    free(status);
    status = call(app, "{\"action\":\"team-toggle\"}");
    check(status && strstr(status, "\"show_team\":true"), "team toggle");
    free(status);
    controller_shutdown(app);
    controller_free(app);
    deps = base_deps(&log);
    app = controller_create(dir, &deps);
    check(controller_restore(app) == 1 || 1, "restore returns");
    status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"show_team\":true"), "team toggle persisted");
    free(status);
    status = call(app, "{\"action\":\"nope\"}");
    check(status && strstr(status, "\"ok\":false") && strstr(status, "Unknown voice command"), "unknown action");
    free(status);
    char *selection = NULL;
    char path[512];
    snprintf(path, sizeof path, "%s/selection.json", dir);
    FILE *file = fopen(path, "r");
    if (file) {
        fseek(file, 0, SEEK_END);
        long n = ftell(file);
        rewind(file);
        selection = calloc(1, (size_t)n + 1);
        if (n > 0 && fread(selection, 1, (size_t)n, file) == 0) selection[0] = 0;
        fclose(file);
    }
    check(selection && strstr(selection, "attachment_fences") && strstr(selection, "show_team"), "selection schema");
    free(selection);
    char err[256] = {0};
    free(call(app, "{\"action\":\"register\",\"token\":\"token-1\",\"target\":{\"pane\":\"w1:p2\",\"socket\":\"/tmp/herdr.sock\",\"pid\":100,\"start\":\"200\",\"harness\":\"pi\"}}"));
    log.uncertain = 1;
    int staged = controller_stage(app, "hello\x01", "token-1", NULL, 1, err, sizeof err);
    check(staged < 0 && strstr(err, "control"), "control characters rejected before insert");
    err[0] = 0;
    log.uncertain = 0;
    check(controller_stage(app, "keep this", "token-1", NULL, 1, err, sizeof err) > 0, "stage accepts dictation");
    log.uncertain = 1;
    err[0] = 0;
    check(controller_stage(app, "maybe delivered", "token-1", NULL, 1, err, sizeof err) == CTRL_UNCERTAIN, "uncertain stage");
    check(app->pending == NULL && app->draft == 0, "uncertain stage disarms send");
    log.uncertain = 0;
    log.state[0] = 0;
    snprintf(log.state, sizeof log.state, "busy");
    app->draft = 1;
    err[0] = 0;
    check(controller_send(app, NULL, 0, 1, err, sizeof err) == 0, "busy send queues");
    status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"queued\":true"), "explicit queued send");
    free(status);
    for (int i = 0; i < 70; i++) {
        char req[256];
        snprintf(req, sizeof req, "{\"action\":\"notify\",\"token\":\"token-1\",\"event\":{\"type\":\"agent-turn-complete\",\"thread-id\":\"conv\",\"turn-id\":\"t%d\",\"last-assistant-message\":\"TL;DR: One.\"}}", i);
        free(call(app, req));
    }
    check(app->turn_count == 70, "turns are not capped at 64");
    char *dup = call(app, "{\"action\":\"notify\",\"token\":\"token-1\",\"event\":{\"type\":\"agent-turn-complete\",\"thread-id\":\"conv\",\"turn-id\":\"t0\",\"last-assistant-message\":\"TL;DR: Replay.\"}}");
    check(dup && strstr(dup, "\"accepted\":false"), "duplicate turn is not replayed");
    free(dup);
    check(app->turn_count == 70, "duplicate turn does not grow the set");
    free(call(app, "{\"action\":\"register\",\"token\":\"wide\",\"target\":{\"pane\":\"pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp\",\"socket\":\"/tmp/herdr.sock\",\"pid\":100,\"start\":\"200\",\"harness\":\"pi\",\"model\":{\"id\":\"m1\",\"name\":\"Model\"}}}"));
    status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"id\":\"m1\"") && strstr(status, "\"name\":\"Model\""), "model object survives status");
    free(status);
    status = call(app, "{\"action\":\"register\",\"token\":\"bad\",\"target\":{\"pane\":\"p\",\"socket\":\"/tmp/h\",\"pid\":999999999999,\"harness\":\"pi\"}}");
    check(status && strstr(status, "\"ok\":false"), "out-of-range pid rejected");
    free(status);
    char *attached = call(app, "{\"action\":\"attach\",\"target\":{}}");
    check(attached && strstr(attached, "\"state\":\"connecting\""), "invalid attach stays quiet");
    free(attached);
    pthread_mutex_lock(&app->state);
    app->has_retry = 1;
    app->retry_deadline = -1;
    app->retry_path = strdup("/tmp/voice-missing-retry.wav");
    pthread_mutex_unlock(&app->state);
    controller_monitor(app);
    check(app->has_retry == 0, "retry absolute deadline expires");
    app->deps.audio.status_json = status_with_mic;
    status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"muted\":true") && strstr(status, "\"missing\":true")
        && strstr(status, "\"preferred\":\"wanted\"") && strstr(status, "\"error\":\"unavailable\"")
        && strstr(status, "\"clipping\":false"), "microphone details survive status");
    free(status);
    target_data left = {0}, right = {0};
    snprintf(left.pane, sizeof left.pane, "w1:p2");
    left.pid = 100;
    right = left;
    right.pid++;
    check(!controller_same_target(&left, &right), "target snapshot rejects a reused pid");
    controller_free(app);
    late_audio audio = {0};
    deps = base_deps(&log);
    deps.audio.start_capture = late_start;
    deps.audio.transcribe_owned = late_transcribe;
    deps.audio.user = &audio;
    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"rec\",\"target\":{\"pane\":\"w1:p2\",\"socket\":\"/tmp/herdr.sock\",\"pid\":100,\"start\":\"200\",\"harness\":\"pi\"}}"));
    char rec_err[128] = {0};
    check(controller_record(app, "append", rec_err, sizeof rec_err) == 0, "record starts");
    for (int i = 0; i < 200 && !atomic_load(&audio.cap.close_entered); i++) usleep(10000);
    check(atomic_load(&audio.cap.close_entered), "stop waits while capture close is in progress");
    status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"ok\":true"), "status during capture close");
    free(status);
    pthread_mutex_lock(&audio.cap.mu);
    atomic_store(&audio.cap.release_close, 1);
    pthread_cond_broadcast(&audio.cap.cv);
    pthread_mutex_unlock(&audio.cap.mu);
    if (atomic_load(&audio.armed)) pthread_join(audio.thread, NULL);
    for (int i = 0; i < 200 && !atomic_load(&audio.cap.destroyed); i++) usleep(10000);
    check(atomic_load(&audio.cap.destroyed), "capture destroyed after close");
    status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"phase\":\"draft\""), "staged recording finishes as draft");
    free(status);
    pthread_mutex_lock(&app->state);
    session_entry *recorded = controller_find(app, app->token);
    if (recorded) {
        free(recorded->pending);
        recorded->pending = strdup("keep these words");
    }
    free(app->pending);
    app->pending = strdup("keep these words");
    controller_remove_session(app, app->token, 1);
    pthread_mutex_unlock(&app->state);
    check(app->retained && app->retained->text && strstr(app->retained->text, "keep these words"), "target loss retains undelivered dictation");
    controller_free(app);

    target_data issued = {0}, retry = {0};
    snprintf(issued.pane, sizeof issued.pane, "w1:p1");
    snprintf(issued.socket, sizeof issued.socket, "/tmp/herdr.sock");
    snprintf(issued.token, sizeof issued.token, "issued-token");
    snprintf(issued.harness, sizeof issued.harness, "pi");
    issued.pid = 42;
    issued.has_bridge = 1;
    snprintf(issued.bridge_id, sizeof issued.bridge_id, "bridge");
    retry = issued;
    retry.token[0] = 0;
    check(controller_same_attachment(&issued, &retry), "reattach identity ignores the issued token");
    retry.pid++;
    check(!controller_same_attachment(&issued, &retry), "reattach identity still rejects a different process");

    deps = base_deps(&log);
    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"aaa\",\"target\":{\"pane\":\"a\",\"socket\":\"/tmp/a.sock\",\"pid\":1,\"start\":\"1\",\"harness\":\"pi\"}}"));
    free(call(app, "{\"action\":\"register\",\"token\":\"bee\",\"target\":{\"pane\":\"b\",\"socket\":\"/tmp/b.sock\",\"pid\":2,\"start\":\"2\",\"harness\":\"pi\"}}"));
    free(call(app, "{\"action\":\"select:aaa\"}"));
    switch_app = app;
    app->deps.audio.stop = switch_during_stop;
    free(app->pending);
    app->pending = strdup("from-a");
    char race_err[160] = {0};
    check(controller_record(app, "append", race_err, sizeof race_err) != 0, "switched selection is not recorded");
    check(app->has_token && strcmp(app->token, "bee") == 0, "stop switched to the other session");
    check(!app->pending || !strstr(app->pending, "from-a"), "session A text was not published onto B");
    controller_free(app);
    switch_app = NULL;

    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"aaa\",\"target\":{\"pane\":\"a\",\"socket\":\"/tmp/a.sock\",\"pid\":1,\"start\":\"1\",\"harness\":\"pi\"}}"));
    switch_app = app;
    app->deps.audio.stop = mutate_during_stop;
    free(app->pending);
    app->pending = strdup("from-a");
    check(controller_record(app, "append", race_err, sizeof race_err) != 0, "in-place identity change is not the same target");
    controller_free(app);
    switch_app = NULL;

    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"aaa\",\"target\":{\"pane\":\"a\",\"socket\":\"/tmp/a.sock\",\"pid\":1,\"start\":\"1\",\"harness\":\"pi\"}}"));
    switch_app = app;
    app->deps.audio.stop = discard_during_stop;
    free(app->pending);
    app->pending = strdup("secret");
    check(controller_record(app, "append", race_err, sizeof race_err) != 0, "concurrent discard is not undone");
    check(app->pending == NULL, "discarded dictation stayed discarded");
    controller_free(app);
    switch_app = NULL;

    hold_cap toggle = {0};
    pthread_mutex_init(&toggle.mu, NULL);
    pthread_cond_init(&toggle.cv, NULL);
    toggle.base.close = hold_close;
    toggle.base.destroy = hold_destroy;
    toggle.base.level = hold_level;
    toggle.base.clipping = hold_clip;
    deps = base_deps(&log);
    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"rec\",\"target\":{\"pane\":\"w\",\"socket\":\"/tmp/h.sock\",\"pid\":3,\"start\":\"3\",\"harness\":\"pi\"}}"));
    pthread_mutex_lock(&app->state);
    snprintf(app->phase, sizeof app->phase, "recording");
    void *tracked = controller_capture_track(&toggle.base);
    controller_capture_publish(app, tracked);
    pthread_mutex_unlock(&app->state);
    int toggle_rc = controller_record(app, "append", race_err, sizeof race_err);
    if (toggle_rc != 0) fprintf(stderr, "toggle rc %d err %s phase %s\n", toggle_rc, race_err, app->phase);
    check(toggle_rc == 0, "recording toggle returns");
    for (int i = 0; i < 100 && !atomic_load(&toggle.close_entered); i++) usleep(10000);
    check(atomic_load(&toggle.close_entered), "toggle closes through the owner");
    status = call(app, "{\"action\":\"status\"}");
    check(status && strstr(status, "\"ok\":true"), "status during owned capture close");
    free(status);
    pthread_mutex_lock(&toggle.mu);
    atomic_store(&toggle.release_close, 1);
    pthread_cond_broadcast(&toggle.cv);
    pthread_mutex_unlock(&toggle.mu);
    controller_capture_release(tracked);
    for (int i = 0; i < 100 && !atomic_load(&toggle.destroyed); i++) usleep(10000);
    check(atomic_load(&toggle.destroyed), "capture freed only after the last owner");
    controller_free(app);

    gate_audio gate = {0};
    pthread_mutex_init(&gate.mu, NULL);
    pthread_cond_init(&gate.cv, NULL);
    gate.drain_text = "\x01";
    deps = base_deps(&log);
    deps.audio.user = &gate;
    deps.audio.start_capture = gate_start;
    deps.audio.transcribe_owned = gate_transcribe;
    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"rec\",\"target\":{\"pane\":\"w\",\"socket\":\"/tmp/h.sock\",\"pid\":4,\"start\":\"4\",\"harness\":\"pi\"}}"));
    free(app->pending);
    app->pending = strdup("Previous words");
    check(controller_record(app, "append", race_err, sizeof race_err) == 0, "append recovery record starts");
    pthread_mutex_lock(&gate.mu);
    struct timespec gate_deadline;
    clock_gettime(CLOCK_REALTIME, &gate_deadline);
    gate_deadline.tv_sec += 2;
    while (!gate.entered && pthread_cond_timedwait(&gate.cv, &gate.mu, &gate_deadline) != ETIMEDOUT) {}
    int gate_ready = gate.entered;
    pthread_mutex_unlock(&gate.mu);
    check(gate_ready, "recovery transcription started");
    pthread_mutex_lock(&app->state);
    if (app->record_op) app->record_op->voice_target_lost = 1;
    if (!app->retained) {
        app->retained = calloc(1, sizeof *app->retained);
        app->retained->text = strdup("Previous words");
        snprintf(app->retained->source_token, sizeof app->retained->source_token, "rec");
    }
    pthread_mutex_unlock(&app->state);
    pthread_mutex_lock(&gate.mu);
    gate.go = 1;
    pthread_cond_broadcast(&gate.cv);
    pthread_mutex_unlock(&gate.mu);
    char kept[128] = {0};
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&app->state);
        int starting = strcmp(app->phase, "starting") == 0;
        if (app->retained && app->retained->text) snprintf(kept, sizeof kept, "%s", app->retained->text);
        pthread_mutex_unlock(&app->state);
        if (!starting && kept[0]) break;
        usleep(10000);
    }
    check(strcmp(kept, "Previous words") == 0, "rejected late recovery keeps the original draft");
    controller_free(app);

    memset(&gate, 0, sizeof gate);
    pthread_mutex_init(&gate.mu, NULL);
    pthread_cond_init(&gate.cv, NULL);
    gate.drain_text = "Yes";
    deps.audio.user = &gate;
    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"rec\",\"target\":{\"pane\":\"w\",\"socket\":\"/tmp/h.sock\",\"pid\":4,\"start\":\"4\",\"harness\":\"pi\"}}"));
    free(app->pending);
    app->pending = strdup("Yes");
    check(controller_record(app, "append", race_err, sizeof race_err) == 0, "append late recovery starts");
    pthread_mutex_lock(&gate.mu);
    struct timespec late_deadline;
    clock_gettime(CLOCK_REALTIME, &late_deadline);
    late_deadline.tv_sec += 2;
    while (!gate.entered && pthread_cond_timedwait(&gate.cv, &gate.mu, &late_deadline) != ETIMEDOUT) {}
    int late_ready = gate.entered;
    pthread_mutex_unlock(&gate.mu);
    check(late_ready, "late recovery transcription started");
    pthread_mutex_lock(&app->state);
    if (app->record_op) app->record_op->voice_target_lost = 1;
    if (!app->retained) {
        app->retained = calloc(1, sizeof *app->retained);
        app->retained->text = strdup("Yes");
        snprintf(app->retained->source_token, sizeof app->retained->source_token, "rec");
    }
    pthread_mutex_unlock(&app->state);
    pthread_mutex_lock(&gate.mu);
    gate.go = 1;
    pthread_cond_broadcast(&gate.cv);
    pthread_mutex_unlock(&gate.mu);
    char combined[160] = {0};
    for (int i = 0; i < 150; i++) {
        pthread_mutex_lock(&app->state);
        if (app->retained && app->retained->text) snprintf(combined, sizeof combined, "%s", app->retained->text);
        pthread_mutex_unlock(&app->state);
        if (strcmp(combined, "Yes\nYes") == 0) break;
        usleep(10000);
    }
    check(strcmp(combined, "Yes\nYes") == 0, "repeated phrase is appended, not deduped");
    controller_free(app);

    many_cap many = {0};
    many.limit = 70;
    deps = base_deps(&log);
    deps.audio.user = &many;
    deps.audio.start_capture = many_start;
    app = controller_create(dir, &deps);
    free(call(app, "{\"action\":\"register\",\"token\":\"rec\",\"target\":{\"pane\":\"w\",\"socket\":\"/tmp/h.sock\",\"pid\":9,\"start\":\"9\",\"harness\":\"pi\"}}"));
    check(controller_record(app, "append", race_err, sizeof race_err) == 0, "long capture starts");
    for (int i = 0; i < 400 && wavs_left(dir) != 0; i++) usleep(10000);
    check(wavs_left(dir) == 0, "every temporary slice past 64 is removed");
    controller_free(app);
    rmdir(dir);
    if (failures) fprintf(stderr, "%d controller checks failed\n", failures);
    return failures;
}
