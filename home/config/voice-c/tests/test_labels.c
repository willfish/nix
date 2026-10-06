#define _POSIX_C_SOURCE 200809L

#include "labels.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;

static void fail(const char *name, const char *detail) {
    fprintf(stderr, "FAIL %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
    failures++;
}

static void expect_true(const char *name, int cond) {
    if (!cond) fail(name, "expected true");
}

static void expect_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d want %d\n", name, got, want);
        failures++;
    }
}

static void expect_str(const char *name, const char *got, const char *want) {
    if (!got || !want || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  got:  [%s]\n  want: [%s]\n", name, got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static int ends_with(const char *text, const char *suffix) {
    if (!text || !suffix) return 0;
    size_t n = strlen(text);
    size_t m = strlen(suffix);
    return n >= m && strcmp(text + n - m, suffix) == 0;
}

static int count_substr(const char *text, const char *needle) {
    int count = 0;
    size_t n = strlen(needle);
    if (!text || !n) return 0;
    for (const char *p = text; (p = strstr(p, needle)); p += n) count++;
    return count;
}

static const char *payload_json =
    "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\"}],"
    "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"shell\"}],"
    "\"panes\":[{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"}]}";

static const char *empty_json =
    "{\"workspaces\":[],\"tabs\":[],\"panes\":[]}";

static const char *malformed_json = "{\"unexpected\":[]}";

typedef struct {
    double now;
    int fail;
    const char *result;
    int calls;
    double timeouts[4];
    SocketKey keys[4];
} Runner;

static int test_runner(const SocketKey *key, double timeout, char **json, void *user) {
    Runner *runner = user;
    if (runner->calls < 4) {
        runner->timeouts[runner->calls] = timeout;
        runner->keys[runner->calls] = *key;
    }
    runner->calls++;
    if (runner->fail) return -1;
    *json = strdup(runner->result ? runner->result : payload_json);
    return *json ? 0 : -1;
}

static double test_clock(void *user) {
    return ((Runner *)user)->now;
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} QueueJob;

static QueueJob jobs[8];
static int job_count;
static pthread_mutex_t job_mu = PTHREAD_MUTEX_INITIALIZER;

static int queue_submit(void (*fn)(void *), void *arg, void *sched) {
    (void)sched;
    pthread_mutex_lock(&job_mu);
    if (job_count >= 8) {
        pthread_mutex_unlock(&job_mu);
        return -1;
    }
    jobs[job_count].fn = fn;
    jobs[job_count].arg = arg;
    job_count++;
    pthread_mutex_unlock(&job_mu);
    return 0;
}

static void queue_run(void) {
    pthread_mutex_lock(&job_mu);
    QueueJob job = jobs[0];
    memmove(jobs, jobs + 1, (size_t)(job_count - 1) * sizeof job);
    job_count--;
    pthread_mutex_unlock(&job_mu);
    job.fn(job.arg);
}

static LabelsCache *make_cache(Runner *runner, int max_keys) {
    LabelsCacheConfig cfg = {
        .runner = test_runner,
        .runner_user = runner,
        .clock = test_clock,
        .clock_user = runner,
        .submit = queue_submit,
        .max_keys = max_keys,
    };
    return labels_cache_new(&cfg);
}

static void test_reads_are_pure_and_single_flight_and_success_ttl(void) {
    Runner runner = {.result = payload_json};
    SocketKey key;
    socket_key_init(&key, "/a", 1, 2);
    LabelsCache *cache = make_cache(&runner, 2);
    expect_int("set active", labels_cache_set_active(cache, &key, 1), LABELS_OK);
    SnapshotState *state = labels_cache_read(cache, &key);
    expect_int("initial outcome", labels_state_outcome(state), LABELS_OUTCOME_UNKNOWN);
    labels_state_free(state);
    expect_int("no jobs yet", job_count, 0);
    expect_true("refresh starts", labels_cache_refresh(cache, &key));
    expect_true("single flight", !labels_cache_refresh(cache, &key));
    state = labels_cache_read(cache, &key);
    expect_true("inflight", labels_state_inflight(state));
    labels_state_free(state);
    queue_run();
    state = labels_cache_read(cache, &key);
    expect_true("authoritative now", labels_state_authoritative(state, runner.now));
    labels_state_free(state);
    expect_true("ttl holds", !labels_cache_refresh(cache, &key));
    runner.now = 3;
    expect_true("ttl expired", labels_cache_refresh(cache, &key));
    queue_run();
    expect_int("runner calls", runner.calls, 2);
    expect_true("timeout is 1", runner.timeouts[0] == 1.0);
    labels_cache_free(cache);
    socket_key_clear(&key);
}

static void test_empty_is_success_and_failures_preserve_stale_not_evidence(void) {
    Runner runner = {.result = payload_json};
    SocketKey key;
    socket_key_init(&key, "/a", 1, 2);
    LabelsCache *cache = make_cache(&runner, 2);
    labels_cache_set_active(cache, &key, 1);
    labels_cache_refresh(cache, &key);
    queue_run();
    runner.now = 3;
    runner.fail = 1;
    labels_cache_refresh(cache, &key);
    queue_run();
    SnapshotState *state = labels_cache_read(cache, &key);
    expect_int("failure outcome", labels_state_outcome(state), LABELS_OUTCOME_UNAVAILABLE);
    expect_str("stale pane", labels_state_field(state, "w1:p1", "workspace"), "dot");
    expect_true("failure is not authority", !labels_state_authoritative(state, runner.now));
    labels_state_free(state);
    expect_true("failure ttl", !labels_cache_refresh(cache, &key));
    runner.now = 6;
    runner.fail = 0;
    runner.result = empty_json;
    labels_cache_refresh(cache, &key);
    queue_run();
    state = labels_cache_read(cache, &key);
    expect_int("empty panes", (int)labels_state_pane_count(state), 0);
    expect_true("empty is authoritative", labels_state_authoritative(state, runner.now));
    labels_state_free(state);
    labels_cache_free(cache);
    socket_key_clear(&key);
}

static void test_malformed_negative_ttl_and_immutable_snapshots(void) {
    Runner runner = {.result = malformed_json};
    SocketKey key;
    socket_key_init(&key, "/a", 1, 2);
    LabelsCache *cache = make_cache(&runner, 2);
    labels_cache_set_active(cache, &key, 1);
    labels_cache_refresh(cache, &key);
    queue_run();
    SnapshotState *state = labels_cache_read(cache, &key);
    expect_int("malformed outcome", labels_state_outcome(state), LABELS_OUTCOME_UNAVAILABLE);
    labels_state_free(state);
    expect_true("malformed ttl", !labels_cache_refresh(cache, &key));
    runner.now = 3;
    runner.result = payload_json;
    labels_cache_refresh(cache, &key);
    queue_run();
    state = labels_cache_read(cache, &key);
    expect_true("mutation rejected", labels_state_set_field(state, "w1:p1", "workspace", "changed") != 0);
    expect_str("immutable workspace", labels_state_field(state, "w1:p1", "workspace"), "dot");
    labels_state_free(state);
    state = labels_cache_read(cache, &key);
    expect_str("cache unchanged", labels_state_field(state, "w1:p1", "workspace"), "dot");
    expect_true("age of 3 is not authoritative", !labels_state_authoritative(state, 6));
    expect_true("negative age is not authoritative", !labels_state_authoritative(state, 2));
    labels_state_free(state);
    labels_cache_free(cache);
    socket_key_clear(&key);
}

static void test_retirement_fences_late_result_and_bounds_pending_work(void) {
    Runner runner = {.result = payload_json};
    SocketKey key;
    socket_key_init(&key, "/a", 1, 2);
    LabelsCache *cache = make_cache(&runner, 2);
    labels_cache_set_active(cache, &key, 1);
    labels_cache_refresh(cache, &key);
    labels_cache_set_active(cache, NULL, 0);
    labels_cache_set_active(cache, &key, 1);
    labels_cache_refresh(cache, &key);
    queue_run();
    SnapshotState *state = labels_cache_read(cache, &key);
    expect_int("late result discarded", labels_state_outcome(state), LABELS_OUTCOME_UNKNOWN);
    labels_state_free(state);
    queue_run();
    state = labels_cache_read(cache, &key);
    expect_int("new ticket published", labels_state_outcome(state), LABELS_OUTCOME_OK);
    labels_state_free(state);
    SocketKey many[3];
    for (int i = 0; i < 3; i++) {
        char path[16];
        snprintf(path, sizeof path, "%d", i);
        socket_key_init(&many[i], path, 1, (uint64_t)i);
    }
    expect_int("too many keys", labels_cache_set_active(cache, many, 3), LABELS_TOO_MANY);
    for (int i = 0; i < 3; i++) socket_key_clear(&many[i]);
    labels_cache_free(cache);
    socket_key_clear(&key);
}

typedef struct {
    uint64_t device[4];
    uint64_t inode[4];
    int count;
    int index;
    int calls;
} StatSeq;

typedef struct {
    int calls;
    double timeout;
    char path[64];
    const char *stdout_text;
} ExecRec;

static int stat_seq(const char *path, uint64_t *device, uint64_t *inode, void *user) {
    (void)path;
    StatSeq *seq = user;
    int i = seq->index < seq->count ? seq->index++ : seq->count - 1;
    seq->calls++;
    *device = seq->device[i];
    *inode = seq->inode[i];
    return 0;
}

static int exec_rec(const char *socket_path, double timeout, char **stdout_text, void *user) {
    ExecRec *exec = user;
    exec->calls++;
    exec->timeout = timeout;
    snprintf(exec->path, sizeof exec->path, "%s", socket_path);
    *stdout_text = strdup(exec->stdout_text);
    return *stdout_text ? 0 : -1;
}

static void test_runner_checks_socket_instance_and_passes_timeout(void) {
    SocketKey key;
    socket_key_init(&key, "/a", 1, 2);
    const char *stdout_text =
        "{\"result\": {\"snapshot\": {\"panes\": [], \"tabs\": [], \"workspaces\": []}}}";
    StatSeq same = {.device = {1}, .inode = {2}, .count = 1};
    ExecRec exec = {.stdout_text = stdout_text};
    labels_set_stat_fn(stat_seq, &same);
    labels_set_exec_fn(exec_rec, &exec);
    char *json = NULL;
    expect_int("snapshot ok", labels_run_snapshot(&key, 1, &json), LABELS_OK);
    SnapshotState *state = labels_parse_snapshot(json);
    expect_true("parsed snapshot", state != NULL);
    expect_int("empty snapshot panes", state ? (int)labels_state_pane_count(state) : -1, 0);
    labels_state_free(state);
    free(json);
    expect_int("stat before and after", same.calls, 2);

    StatSeq changed = {.device = {1, 1}, .inode = {2, 3}, .count = 2};
    ExecRec exec2 = {.stdout_text = stdout_text};
    labels_set_stat_fn(stat_seq, &changed);
    labels_set_exec_fn(exec_rec, &exec2);
    json = NULL;
    expect_true("instance changed", labels_run_snapshot(&key, 1, &json) != 0);
    expect_true("error names instance", strstr(labels_last_error(), "instance changed") != NULL);
    expect_true("timeout passed", exec2.timeout == 1.0);
    expect_str("socket path env", exec2.path, "/a");
    expect_int("command ran", exec2.calls, 1);
    free(json);

    StatSeq early = {.device = {1}, .inode = {3}, .count = 1};
    ExecRec exec3 = {.stdout_text = stdout_text};
    labels_set_stat_fn(stat_seq, &early);
    labels_set_exec_fn(exec_rec, &exec3);
    expect_true("pre-stat mismatch", labels_run_snapshot(&key, 1, &json) != 0);
    expect_int("command not called", exec3.calls, 0);
    labels_set_stat_fn(NULL, NULL);
    labels_set_exec_fn(NULL, NULL);
    socket_key_clear(&key);
}

/* The real runner shells out to herdr, so the pipe reader has to survive output
 * larger than one read and a child that closes stdout before exiting. */
static char fake_path[1024];

static int write_fake_herdr(const char *dir, const char *body) {
    snprintf(fake_path, sizeof fake_path, "%s/herdr", dir);
    FILE *file = fopen(fake_path, "w");
    if (!file) return -1;
    if (chmod(fake_path, 0700) != 0) {
        fclose(file);
        return -1;
    }
    int ok = fprintf(file, "#!/bin/sh\n%s\n", body) > 0;
    if (fclose(file) != 0) return -1;
    return ok ? 0 : -1;
}

#define FAKE_HERDR_BODY \
    "printf '{\"result\":{\"snapshot\":{\"panes\":['\n" \
    "i=0\n" \
    "while [ $i -lt 400 ]; do\n" \
    "  if [ $i -gt 0 ]; then printf ','; fi\n" \
    "  printf '{\"pane_id\":\"w9:p%s\",\"tab\":\"padding-padding-padding-padding-padding\"}' \"$i\"\n" \
    "  i=$((i + 1))\n" \
    "done\n" \
    "printf '],\"tabs\":[],\"workspaces\":[]}}}'\n"

static void test_real_snapshot_runner_reaps_the_child(void) {
    char dir[] = "/tmp/voice-labels-runner-XXXXXX";
    if (!mkdtemp(dir)) {
        fail("snapshot runner", "mkdtemp");
        return;
    }
    char sock_path[sizeof dir + sizeof "/herdr.sock"];
    snprintf(sock_path, sizeof sock_path, "%s/herdr.sock", dir);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    struct stat st;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || stat(sock_path, &st) != 0) {
        fail("snapshot runner", "socket setup");
        if (fd >= 0) close(fd);
        return;
    }
    char path_env[4096];
    const char *old_path = getenv("PATH");
    snprintf(path_env, sizeof path_env, "%s%s%s", dir, old_path && old_path[0] ? ":" : "",
        old_path ? old_path : "");
    if (setenv("PATH", path_env, 1) != 0) {
        fail("snapshot runner", "path setup");
        close(fd);
        return;
    }
    SocketKey key;
    socket_key_init(&key, sock_path, (uint64_t)st.st_dev, (uint64_t)st.st_ino);
    char *json = NULL;

    if (write_fake_herdr(dir, FAKE_HERDR_BODY) == 0) {
        json = NULL;
        int rc = labels_run_snapshot(&key, 5.0, &json);
        expect_int("multi-read snapshot ok", rc, LABELS_OK);
        SnapshotState *state = json ? labels_parse_snapshot(json) : NULL;
        expect_int("multi-read panes parsed", state ? (int)labels_state_pane_count(state) : -1, 400);
        labels_state_free(state);
        free(json);
    } else {
        fail("snapshot runner", "write fake herdr");
    }

    /* Output flushed, stdout closed, child still running: EOF is not a timeout. */
    if (write_fake_herdr(dir, FAKE_HERDR_BODY "exec 1>&-\nsleep 0.2\nexit 0") == 0) {
        json = NULL;
        int rc = labels_run_snapshot(&key, 5.0, &json);
        expect_int("closed stdout snapshot ok", rc, LABELS_OK);
        SnapshotState *state = json ? labels_parse_snapshot(json) : NULL;
        expect_int("closed stdout panes parsed", state ? (int)labels_state_pane_count(state) : -1, 400);
        labels_state_free(state);
        free(json);
    } else {
        fail("snapshot runner", "write closed stdout fake");
    }

    if (write_fake_herdr(dir, "exit 3") == 0) {
        json = NULL;
        expect_true("failing herdr rejected", labels_run_snapshot(&key, 5.0, &json) != LABELS_OK);
        expect_true("failure is not a timeout", strstr(labels_last_error(), "timed out") == NULL);
        free(json);
    }

    /* A child that never finishes still hits the deadline and is reaped. */
    if (write_fake_herdr(dir, "exec 1>&-\nsleep 30") == 0) {
        json = NULL;
        expect_true("slow herdr times out", labels_run_snapshot(&key, 0.5, &json) != LABELS_OK);
        expect_true("timeout is named", strstr(labels_last_error(), "timed out") != NULL);
        free(json);
    }

    if (old_path) setenv("PATH", old_path, 1);
    else unsetenv("PATH");
    close(fd);
    unlink(sock_path);
    unlink(fake_path);
    rmdir(dir);
    socket_key_clear(&key);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int need;
    int arrived;
    int generation;
} Barrier;

static void barrier_init(Barrier *barrier, int need) {
    pthread_mutex_init(&barrier->mu, NULL);
    pthread_cond_init(&barrier->cv, NULL);
    barrier->need = need;
    barrier->arrived = 0;
    barrier->generation = 0;
}

static void barrier_wait(Barrier *barrier) {
    pthread_mutex_lock(&barrier->mu);
    int generation = barrier->generation;
    barrier->arrived++;
    if (barrier->arrived == barrier->need) {
        barrier->generation++;
        barrier->arrived = 0;
        pthread_cond_broadcast(&barrier->cv);
    } else {
        while (generation == barrier->generation) pthread_cond_wait(&barrier->cv, &barrier->mu);
    }
    pthread_mutex_unlock(&barrier->mu);
}

typedef struct {
    Barrier *barrier;
    LabelsCache *cache;
    SocketKey *key;
} RefreshCtx;

static void *refresh_thread(void *arg) {
    RefreshCtx *ctx = arg;
    barrier_wait(ctx->barrier);
    labels_cache_refresh(ctx->cache, ctx->key);
    return NULL;
}

static void test_simultaneous_refreshes_only_schedule_once(void) {
    Runner runner = {.result = payload_json};
    SocketKey key;
    socket_key_init(&key, "/a", 1, 2);
    LabelsCache *cache = make_cache(&runner, 2);
    labels_cache_set_active(cache, &key, 1);
    Barrier barrier;
    barrier_init(&barrier, 3);
    RefreshCtx ctx = {.barrier = &barrier, .cache = cache, .key = &key};
    pthread_t threads[2];
    pthread_create(&threads[0], NULL, refresh_thread, &ctx);
    pthread_create(&threads[1], NULL, refresh_thread, &ctx);
    barrier_wait(&barrier);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    expect_int("one scheduled refresh", job_count, 1);
    queue_run();
    expect_int("one runner call", runner.calls, 1);
    pthread_mutex_destroy(&barrier.mu);
    pthread_cond_destroy(&barrier.cv);
    labels_cache_free(cache);
    socket_key_clear(&key);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int set;
} Event;

static void event_init(Event *event) {
    pthread_mutex_init(&event->mu, NULL);
    pthread_cond_init(&event->cv, NULL);
    event->set = 0;
}

static void event_set(Event *event) {
    pthread_mutex_lock(&event->mu);
    event->set = 1;
    pthread_cond_broadcast(&event->cv);
    pthread_mutex_unlock(&event->mu);
}

static int event_wait(Event *event, int seconds) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += seconds;
    pthread_mutex_lock(&event->mu);
    int rc = 0;
    while (!event->set && rc == 0) rc = pthread_cond_timedwait(&event->cv, &event->mu, &ts);
    int ok = event->set;
    pthread_mutex_unlock(&event->mu);
    return ok;
}

typedef struct {
    SocketKey first;
    SocketKey second;
    Event *entered;
    Event *release;
    Event *other_done;
} BlockCtx;

static int blocking_runner(const SocketKey *key, double timeout, char **json, void *user) {
    (void)timeout;
    BlockCtx *ctx = user;
    if (socket_key_equal(key, &ctx->first)) {
        event_set(ctx->entered);
        if (!event_wait(ctx->release, 2)) return -1;
    } else {
        event_set(ctx->other_done);
    }
    *json = strdup(payload_json);
    return *json ? 0 : -1;
}

static void test_blocked_worker_does_not_block_reads_or_other_socket(void) {
    Event entered, release, other_done;
    event_init(&entered);
    event_init(&release);
    event_init(&other_done);
    BlockCtx ctx = {.entered = &entered, .release = &release, .other_done = &other_done};
    socket_key_init(&ctx.first, "/a", 1, 2);
    socket_key_init(&ctx.second, "/b", 1, 3);
    LabelsCacheConfig cfg = {.runner = blocking_runner, .runner_user = &ctx, .max_keys = 2};
    LabelsCache *cache = labels_cache_new(&cfg);
    SocketKey keys[2] = {ctx.first, ctx.second};
    int ok = cache && labels_cache_set_active(cache, keys, 2) == LABELS_OK;
    if (ok) ok = labels_cache_refresh(cache, &ctx.first);
    if (ok) ok = event_wait(&entered, 2);
    if (ok) {
        SnapshotState *state = labels_cache_read(cache, &ctx.first);
        ok = state && labels_state_inflight(state) && !labels_cache_refresh(cache, &ctx.first);
        labels_state_free(state);
    }
    if (ok) ok = labels_cache_refresh(cache, &ctx.second) && event_wait(&other_done, 2);
    event_set(&release);
    labels_cache_free(cache);
    if (!ok) fail("blocked worker", "read or other socket stalled");
    socket_key_clear(&ctx.first);
    socket_key_clear(&ctx.second);
    pthread_mutex_destroy(&entered.mu);
    pthread_cond_destroy(&entered.cv);
    pthread_mutex_destroy(&release.mu);
    pthread_cond_destroy(&release.cv);
    pthread_mutex_destroy(&other_done.mu);
    pthread_cond_destroy(&other_done.cv);
}

static LabelEntry pi_row(const char *pane, const char *path, uint64_t device, uint64_t inode) {
    LabelEntry entry;
    memset(&entry, 0, sizeof entry);
    entry.harness = (char *)"pi";
    entry.pane = (char *)pane;
    entry.token = (char *)pane;
    entry.id = (char *)"conversation";
    entry.has_socket_key = 1;
    entry.socket_key.path = (char *)path;
    entry.socket_key.device = device;
    entry.socket_key.inode = inode;
    return entry;
}

static void test_preferred_and_legacy_unchanged(void) {
    LabelEntry entry = pi_row("w1:p1", "/a", 1, 2);
    entry.has_model = 1;
    entry.model_is_dict = 1;
    entry.model_id = (char *)"gpt-6-astra";
    entry.thinking = (char *)"medium";
    LabelEntry legacy;
    memset(&legacy, 0, sizeof legacy);
    legacy.harness = (char *)"legacy";
    legacy.label = (char *)"legacy";
    legacy.id = (char *)"old";
    legacy.label_present = 1;
    legacy.pane = (char *)"";
    LabelEntry in[2] = {entry, legacy};
    SnapshotState *state = labels_parse_snapshot(payload_json);
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    expect_int("build", labels_build(in, 2, &entry.socket_key, states, 1, &out, &n), LABELS_OK);
    expect_str("preferred", out[0].label, "pi · dot · shell · medium · gpt-6-astra");
    expect_str("legacy label", out[1].label, "legacy");
    expect_str("legacy harness", out[1].harness, "legacy");
    expect_str("legacy id", out[1].id, "old");
    expect_true("legacy full absent", out[1].full_label == NULL);
    expect_str("id preserved", out[0].id, "conversation");
    expect_true("input not labelled", entry.label == NULL);
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void test_named_tab_is_kept_without_workspace_peers(void) {
    const char *json =
        "{\"workspaces\":["
        "{\"workspace_id\":\"w1\",\"label\":\"frontend\"},"
        "{\"workspace_id\":\"w2\",\"label\":\"dot\"}],"
        "\"tabs\":["
        "{\"tab_id\":\"t1\",\"label\":\"polling\"},"
        "{\"tab_id\":\"t2\",\"label\":\"voice\"}],"
        "\"panes\":["
        "{\"pane_id\":\"w44:p1H\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"},"
        "{\"pane_id\":\"w4D:p3\",\"workspace_id\":\"w2\",\"tab_id\":\"t2\"}]}";
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry polling = pi_row("w44:p1H", "/a", 1, 2);
    polling.has_model = 1;
    polling.model = (char *)"gpt-6-astra";
    polling.thinking = (char *)"medium";
    LabelEntry other = pi_row("w4D:p3", "/a", 1, 2);
    SnapshotState *states[1] = {state};
    for (int count = 1; count <= 2; count++) {
        LabelEntry in[2] = {polling, other};
        LabelEntry *out = NULL;
        size_t n = 0;
        expect_int("named build", labels_build(in, (size_t)count, &polling.socket_key, states, 1, &out, &n), LABELS_OK);
        expect_str("named tab", out ? out[0].label : NULL,
            "pi · frontend · polling · medium · gpt-6-astra");
        expect_true("no pane id", out && out[0].full_label && !strstr(out[0].full_label, "p1H"));
        labels_entries_free(out, n);
    }
    labels_state_free(state);
}

static void test_different_named_tabs_need_no_pane_suffix(void) {
    const char *json =
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"frontend\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"polling\"},{\"tab_id\":\"t2\",\"label\":\"stories\"}],"
        "\"panes\":["
        "{\"pane_id\":\"w44:p1H\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"},"
        "{\"pane_id\":\"w44:p2N\",\"workspace_id\":\"w1\",\"tab_id\":\"t2\"}]}";
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry in[2] = {pi_row("w44:p1H", "/a", 1, 2), pi_row("w44:p2N", "/a", 1, 2)};
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(in, 2, &in[0].socket_key, states, 1, &out, &n);
    expect_str("polling", out[0].label, "pi · frontend · polling");
    expect_str("stories", out[1].label, "pi · frontend · stories");
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void test_tab_without_workspace_still_provides_a_name(void) {
    const char *json =
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"polling\"}],"
        "\"panes\":[{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"}]}";
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry entry = pi_row("w1:p1", "/a", 1, 2);
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(&entry, 1, &entry.socket_key, states, 1, &out, &n);
    expect_str("tab name", out[0].label, "pi · polling");
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void test_truncated_names_still_get_unique_suffixes(void) {
    char polling[128], stories[128];
    polling[0] = stories[0] = 0;
    for (int i = 0; i < 10; i++) {
        strcat(polling, "long tab ");
        strcat(stories, "long tab ");
    }
    strcat(polling, "polling");
    strcat(stories, "stories");
    char json[1024];
    snprintf(json, sizeof json,
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"%s\"},{\"tab_id\":\"t2\",\"label\":\"%s\"}],"
        "\"panes\":["
        "{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"},"
        "{\"pane_id\":\"w1:p2\",\"workspace_id\":\"w1\",\"tab_id\":\"t2\"}]}",
        polling, stories);
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry in[2] = {pi_row("w1:p1", "/a", 1, 2), pi_row("w1:p2", "/a", 1, 2)};
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(in, 2, &in[0].socket_key, states, 1, &out, &n);
    expect_true("truncated labels differ", out && strcmp(out[0].label, out[1].label) != 0);
    expect_true("keeps p1", out && ends_with(out[0].label, "p1"));
    expect_true("keeps p2", out && ends_with(out[1].label, "p2"));
    expect_true("p1 width", out && labels_display_width(out[0].label) <= 55);
    expect_true("p2 width", out && labels_display_width(out[1].label) <= 55);
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void test_full_pane_fallback_and_cross_server_discriminator(void) {
    LabelEntry in[3] = {
        pi_row("w1:p1", "/a", 1, 2),
        pi_row("w2:p1", "/a", 1, 2),
        pi_row("w1:p1", "/b", 2, 3),
    };
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(in, 3, NULL, NULL, 0, &out, &n);
    expect_str("server a", out[0].label, "pi · w1:p1@2a5530");
    expect_str("other pane", out[1].label, "pi · w2:p1");
    expect_str("server b", out[2].label, "pi · w1:p1@84f48c");
    expect_true("contains pane", out && strstr(out[0].label, "w") && strstr(out[0].label, ":p1")
        && strstr(out[1].label, "w") && strstr(out[1].label, ":p1")
        && strstr(out[2].label, "w") && strstr(out[2].label, ":p1"));
    LabelEntry *again = NULL;
    size_t n2 = 0;
    labels_build(in, 3, NULL, NULL, 0, &again, &n2);
    expect_true("stable", again && strcmp(out[0].label, again[0].label) == 0
        && strcmp(out[1].label, again[1].label) == 0
        && strcmp(out[2].label, again[2].label) == 0);
    labels_entries_free(out, n);
    labels_entries_free(again, n2);
}

static void test_duplicate_tabs_use_full_panes_when_short_pane_collides(void) {
    const char *json =
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"shell\"}],"
        "\"panes\":["
        "{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"},"
        "{\"pane_id\":\"w2:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"}]}";
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry in[2] = {pi_row("w1:p1", "/a", 1, 2), pi_row("w2:p1", "/a", 1, 2)};
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(in, 2, &in[0].socket_key, states, 1, &out, &n);
    expect_true("shell kept", out && strstr(out[0].full_label, "shell"));
    expect_true("full w1", out && strstr(out[0].label, "w1:p1"));
    expect_true("full w2", out && strstr(out[1].label, "w2:p1"));
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void append_json_string(char *dst, size_t cap, const char *src) {
    size_t n = strlen(dst);
    if (n + 2 >= cap) return;
    dst[n++] = '"';
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            dst[n++] = '\\';
            dst[n++] = (char)*p;
        } else if (*p == '\n') {
            dst[n++] = '\\';
            dst[n++] = 'n';
        } else if (*p < 0x20) {
            n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", *p);
        } else {
            dst[n++] = (char)*p;
        }
    }
    dst[n++] = '"';
    dst[n] = 0;
}

static void test_unicode_controls_suffix_and_long_name_collisions(void) {
    char unit[16];
    snprintf(unit, sizeof unit, "%s", "\xe7\x95\x8c" "e" "\xcc\x81" "\n\x1b");
    char workspace[1024];
    workspace[0] = 0;
    for (int i = 0; i < 80; i++) strcat(workspace, unit);
    char escaped[2048];
    escaped[0] = 0;
    append_json_string(escaped, sizeof escaped, workspace);
    char json[4096];
    snprintf(json, sizeof json,
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":%s}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"shell\"}],"
        "\"panes\":["
        "{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"},"
        "{\"pane_id\":\"w1:p2\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"}]}",
        escaped);
    SnapshotState *state = labels_parse_snapshot(json);
    expect_true("unicode snapshot", state != NULL);
    LabelEntry in[2] = {pi_row("w1:p1", "/a", 1, 2), pi_row("w1:p2", "/a", 1, 2)};
    in[0].selected = 1;
    in[0].team_child = 1;
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(in, 2, &in[0].socket_key, states, 1, &out, &n);
    expect_true("unicode labels differ", out && strcmp(out[0].label, out[1].label) != 0);
    for (int i = 0; out && i < 2; i++) {
        expect_true("unicode width", labels_display_width(out[i].label) <= 55);
        expect_true("no newline", !strstr(out[i].full_label, "\n"));
        expect_true("no escape", !strchr(out[i].full_label, '\x1b'));
    }
    expect_true("team flag", out && strstr(out[0].label, "team"));
    expect_true("p1 suffix", out && strstr(out[0].label, "p1"));
    expect_true("p2 suffix", out && strstr(out[1].label, "p2"));
    expect_int("combining width", labels_display_width("\xe7\x95\x8c" "e" "\xcc\x81"), 3);
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void test_renamed_pane_is_shown_without_replacing_the_tab(void) {
    const char *json =
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"1 voice\"}],"
        "\"panes\":[{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\","
        "\"label\":\"review\",\"title\":\"ignored when label is set\","
        "\"terminal_title\":\"\xcf\x80 - .dotfiles\"}]}";
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry entry = pi_row("w1:p1", "/a", 1, 2);
    entry.has_model = 1;
    entry.model = (char *)"grok-4.7";
    entry.thinking = (char *)"medium";
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(&entry, 1, &entry.socket_key, states, 1, &out, &n);
    expect_str("review label", out[0].label, "pi · dot · 1 voice · review · medium · grok-4.7");
    expect_true("title ignored", !strstr(out[0].label, ".dotfiles"));
    labels_entries_free(out, n);
    labels_state_free(state);
    const char *renamed =
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"1 voice\"}],"
        "\"panes\":[{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\","
        "\"label\":null,\"title\":\"notes\"}]}";
    state = labels_parse_snapshot(renamed);
    states[0] = state;
    labels_build(&entry, 1, &entry.socket_key, states, 1, &out, &n);
    expect_true("notes shown", out && strstr(out[0].label, "notes"));
    expect_true("review gone", out && !strstr(out[0].label, "review"));
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void test_pane_name_matching_the_tab_is_not_repeated(void) {
    const char *json =
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"shell\"}],"
        "\"panes\":[{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\",\"label\":\"shell\"}]}";
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry entry = pi_row("w1:p1", "/a", 1, 2);
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(&entry, 1, &entry.socket_key, states, 1, &out, &n);
    expect_int("shell once", count_substr(out[0].label, "shell"), 1);
    labels_entries_free(out, n);
    labels_state_free(state);
}

static void test_scheduler_rejection_is_unavailable_not_inflight(void) {
    Runner runner = {.result = payload_json, .now = 10};
    SocketKey key;
    socket_key_init(&key, "/a", 1, 2);
    LabelsCache *cache = make_cache(&runner, 1);
    labels_cache_set_active(cache, &key, 1);
    job_count = 8;
    expect_true("rejection reports scheduled", labels_cache_refresh(cache, &key));
    SnapshotState *state = labels_cache_read(cache, &key);
    expect_int("rejection outcome", labels_state_outcome(state), LABELS_OUTCOME_UNAVAILABLE);
    expect_true("rejection not inflight", state && !labels_state_inflight(state));
    labels_state_free(state);
    expect_true("failure ttl holds", !labels_cache_refresh(cache, &key));
    runner.now = 13;
    job_count = 0;
    expect_true("retry after ttl", labels_cache_refresh(cache, &key));
    expect_int("retry queued once", job_count, 1);
    queue_run();
    state = labels_cache_read(cache, &key);
    expect_int("retry published", labels_state_outcome(state), LABELS_OUTCOME_OK);
    labels_state_free(state);
    labels_cache_free(cache);
    socket_key_clear(&key);
    job_count = 0;
}

static void test_identity_hash_matches_python_json(void) {
    LabelEntry del_path = pi_row("w1:p1", "/tmp/a\x7f" "b", 1, 2);
    LabelEntry del_other = pi_row("w1:p1", "/tmp/a\x7f" "bx", 1, 2);
    LabelEntry plain[2] = {del_path, del_other};
    LabelEntry *out = NULL;
    size_t n = 0;
    expect_int("del hash build", labels_build(plain, 2, NULL, NULL, 0, &out, &n), LABELS_OK);
    expect_str("del path digest", out ? out[0].label : NULL, "pi \xc2\xb7 w1:p1@48d039");
    expect_str("del other digest", out ? out[1].label : NULL, "pi \xc2\xb7 w1:p1@ef4a37");
    labels_entries_free(out, n);
    {
        LabelEntry accent = pi_row("w1:p1", "/tmp/h\xc3\xa9llo", 1, 2);
        LabelEntry accent_other = pi_row("w1:p1", "/tmp/h\xc3\xa9llox", 1, 2);
        LabelEntry rows[2] = {accent, accent_other};
        out = NULL;
        n = 0;
        expect_int("unicode hash build", labels_build(rows, 2, NULL, NULL, 0, &out, &n), LABELS_OK);
        expect_str("unicode digest", out ? out[0].label : NULL, "pi \xc2\xb7 w1:p1@b7a5fb");
        labels_entries_free(out, n);
    }
}

static void test_display_agent_fallback_is_pure(void) {
    const char *json =
        "{\"workspaces\":[{\"workspace_id\":\"w1\",\"label\":\"dot\"}],"
        "\"tabs\":[{\"tab_id\":\"t1\",\"label\":\"shell\"}],"
        "\"panes\":[{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\","
        "\"display_agent\":\"pi \xc2\xb7 high \xc2\xb7 provider/model\"}]}";
    SnapshotState *state = labels_parse_snapshot(json);
    LabelEntry entry = pi_row("w1:p1", "/a", 1, 2);
    SnapshotState *states[1] = {state};
    LabelEntry *out = NULL;
    size_t n = 0;
    labels_build(&entry, 1, &entry.socket_key, states, 1, &out, &n);
    expect_str("agent fallback", out[0].label, "pi · dot · shell · high · provider/model");
    expect_true("input unchanged", entry.label == NULL);
    labels_entries_free(out, n);
    labels_state_free(state);
}

int test_labels(void) {
    failures = 0;
    job_count = 0;
    test_reads_are_pure_and_single_flight_and_success_ttl();
    test_empty_is_success_and_failures_preserve_stale_not_evidence();
    test_malformed_negative_ttl_and_immutable_snapshots();
    test_retirement_fences_late_result_and_bounds_pending_work();
    test_runner_checks_socket_instance_and_passes_timeout();
    test_real_snapshot_runner_reaps_the_child();
    test_simultaneous_refreshes_only_schedule_once();
    test_blocked_worker_does_not_block_reads_or_other_socket();
    test_preferred_and_legacy_unchanged();
    test_named_tab_is_kept_without_workspace_peers();
    test_different_named_tabs_need_no_pane_suffix();
    test_tab_without_workspace_still_provides_a_name();
    test_truncated_names_still_get_unique_suffixes();
    test_full_pane_fallback_and_cross_server_discriminator();
    test_duplicate_tabs_use_full_panes_when_short_pane_collides();
    test_unicode_controls_suffix_and_long_name_collisions();
    test_renamed_pane_is_shown_without_replacing_the_tab();
    test_pane_name_matching_the_tab_is_not_repeated();
    test_display_agent_fallback_is_pure();
    test_scheduler_rejection_is_unavailable_not_inflight();
    test_identity_hash_matches_python_json();
    return failures;
}

#ifdef TEST_LABELS_MAIN
int main(void) {
    alarm(30);
    int failed = test_labels();
    if (failed) fprintf(stderr, "%d failures\n", failed);
    return failed ? 1 : 0;
}
#endif
