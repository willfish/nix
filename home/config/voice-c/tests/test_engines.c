#define _POSIX_C_SOURCE 200809L
#include "engines.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static const char *test_name;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, test_name, #cond); \
        failures++; \
    } \
} while (0)

typedef struct gate {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int set;
} gate;

static void gate_init(gate *g) {
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
    g->set = 0;
}

static void gate_destroy(gate *g) {
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->mu);
}

static void gate_set(gate *g) {
    pthread_mutex_lock(&g->mu);
    g->set = 1;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static int gate_is_set(gate *g) {
    pthread_mutex_lock(&g->mu);
    int set = g->set;
    pthread_mutex_unlock(&g->mu);
    return set;
}

static int gate_wait(gate *g, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g->mu);
    while (!g->set) {
        if (pthread_cond_timedwait(&g->cv, &g->mu, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&g->mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g->mu);
    return 1;
}

typedef struct job_queue {
    pthread_mutex_t mu;
    void (*fns[64])(void *);
    void *args[64];
    int n;
} job_queue;

static int q_submit(void (*fn)(void *), void *arg, void *ctx) {
    job_queue *q = ctx;
    pthread_mutex_lock(&q->mu);
    if (q->n >= 64) {
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    q->fns[q->n] = fn;
    q->args[q->n] = arg;
    q->n++;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

static void q_run(job_queue *q) {
    pthread_mutex_lock(&q->mu);
    if (q->n <= 0) {
        pthread_mutex_unlock(&q->mu);
        fprintf(stderr, "FAIL %s: queue_run on empty\n", test_name);
        failures++;
        return;
    }
    void (*fn)(void *) = q->fns[0];
    void *arg = q->args[0];
    memmove(q->fns, q->fns + 1, (size_t)(q->n - 1) * sizeof q->fns[0]);
    memmove(q->args, q->args + 1, (size_t)(q->n - 1) * sizeof q->args[0]);
    q->n--;
    pthread_mutex_unlock(&q->mu);
    fn(arg);
}

static void q_drain(job_queue *q) {
    for (;;) {
        pthread_mutex_lock(&q->mu);
        int pending = q->n;
        pthread_mutex_unlock(&q->mu);
        if (pending <= 0) return;
        q_run(q);
    }
}

static void *q_run_thread(void *arg) {
    q_run(arg);
    return NULL;
}

typedef struct test_timer {
    engine_timer base;
    void (*fn)(void *);
    void *arg;
    int cancelled;
    double delay;
} test_timer;

static void test_timer_cancel(engine_timer *timer) {
    ((test_timer *)timer)->cancelled = 1;
}

static void timer_fire(test_timer *timer) {
    if (timer && !timer->cancelled && timer->fn) timer->fn(timer->arg);
}

typedef struct harness {
    job_queue queue;
    job_queue readies;
    double now;
    struct {
        char engine[8];
        char command[16];
        double timeout;
    } cmds[32];
    int ncmd;
    double last_ready_timeout;
    struct {
        char engine[8];
        double timeout;
    } queries[8];
    int nqueries;
    int fail_runner;
    char fail_msg[64];
    int block_all;
    int block_stop;
    gate entered;
    gate release_gate;
    int fail_later;
    int tts_offline;
    int boom;
    test_timer *timers[16];
    int ntimers;
    engine_manager *manager;
} harness;

static int h_runner(
    const char *engine, const char *command, double timeout, void *ctx,
    char *err, size_t err_cap
) {
    harness *h = ctx;
    if (h->fail_runner) {
        snprintf(err, err_cap, "%s", h->fail_msg[0] ? h->fail_msg : "command failed");
        return -1;
    }
    if (h->ncmd < 32) {
        snprintf(h->cmds[h->ncmd].engine, sizeof h->cmds[h->ncmd].engine, "%s", engine);
        snprintf(h->cmds[h->ncmd].command, sizeof h->cmds[h->ncmd].command, "%s", command);
        h->cmds[h->ncmd].timeout = timeout;
        h->ncmd++;
    }
    if (h->block_all || (h->block_stop && strcmp(command, "stop") == 0)) {
        gate_set(&h->entered);
        if (!gate_wait(&h->release_gate, 2000)) {
            snprintf(err, err_cap, "test barrier");
            return -1;
        }
    }
    return 0;
}

static int h_ready(const char *engine, double timeout, void *ctx, char *err, size_t err_cap) {
    harness *h = ctx;
    h->last_ready_timeout = timeout;
    if (h->tts_offline && strcmp(engine, "tts") == 0) {
        snprintf(err, err_cap, "offline");
        return -1;
    }
    (void)engine;
    return 1;
}

static double h_clock(void *ctx) {
    return ((harness *)ctx)->now;
}

static int h_later(
    double delay, void (*fn)(void *), void *arg, void *ctx,
    engine_timer **out, char *err, size_t err_cap
) {
    harness *h = ctx;
    if (h->fail_later) {
        snprintf(err, err_cap, "timer failed");
        return -1;
    }
    test_timer *timer = calloc(1, sizeof *timer);
    if (!timer) {
        snprintf(err, err_cap, "out of memory");
        return -1;
    }
    timer->base.cancel = test_timer_cancel;
    timer->fn = fn;
    timer->arg = arg;
    timer->delay = delay;
    if (h->ntimers < 16) h->timers[h->ntimers++] = timer;
    *out = &timer->base;
    return 0;
}

static void record_query(harness *h, const char *engine, double timeout) {
    if (h->nqueries >= 8) return;
    snprintf(h->queries[h->nqueries].engine, sizeof h->queries[h->nqueries].engine, "%s", engine);
    h->queries[h->nqueries].timeout = timeout;
    h->nqueries++;
}

static int query_stt(const char *engine, double timeout, void *ctx, int *active, char *err, size_t err_cap) {
    (void)err;
    (void)err_cap;
    record_query(ctx, engine, timeout);
    *active = strcmp(engine, "stt") == 0;
    return 0;
}

static int query_tts(const char *engine, double timeout, void *ctx, int *active, char *err, size_t err_cap) {
    (void)err;
    (void)err_cap;
    record_query(ctx, engine, timeout);
    *active = strcmp(engine, "tts") == 0;
    return 0;
}

static int query_boom(const char *engine, double timeout, void *ctx, int *active, char *err, size_t err_cap) {
    harness *h = ctx;
    (void)engine;
    (void)timeout;
    (void)active;
    (void)err;
    (void)err_cap;
    h->boom = 1;
    return -1;
}

static int query_unavailable(const char *engine, double timeout, void *ctx, int *active, char *err, size_t err_cap) {
    (void)engine;
    (void)timeout;
    (void)ctx;
    (void)active;
    snprintf(err, err_cap, "query unavailable");
    return -1;
}

static void setup(harness *h) {
    memset(h, 0, sizeof *h);
    pthread_mutex_init(&h->queue.mu, NULL);
    pthread_mutex_init(&h->readies.mu, NULL);
    gate_init(&h->entered);
    gate_init(&h->release_gate);
    engine_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.runner = h_runner;
    cfg.runner_ctx = h;
    cfg.readiness = h_ready;
    cfg.readiness_ctx = h;
    cfg.clock = h_clock;
    cfg.clock_ctx = h;
    cfg.submit_command = q_submit;
    cfg.submit_command_ctx = &h->queue;
    cfg.submit_readiness = q_submit;
    cfg.submit_readiness_ctx = &h->readies;
    cfg.call_later = h_later;
    cfg.call_later_ctx = h;
    h->manager = engine_manager_create(&cfg);
    CHECK(h->manager != NULL);
}

static void teardown(harness *h) {
    h->block_all = 0;
    h->block_stop = 0;
    h->fail_runner = 0;
    h->fail_later = 0;
    gate_set(&h->release_gate);
    if (h->manager) {
        q_drain(&h->queue);
        q_drain(&h->readies);
        engine_manager_close(h->manager);
        engine_manager_free(h->manager);
        h->manager = NULL;
    }
    for (int i = 0; i < h->ntimers; i++) free(h->timers[i]);
    pthread_mutex_destroy(&h->queue.mu);
    pthread_mutex_destroy(&h->readies.mu);
    gate_destroy(&h->entered);
    gate_destroy(&h->release_gate);
}

static void become_ready(harness *h, engine_lease *lease) {
    q_drain(&h->queue);
    q_drain(&h->readies);
    char err[256];
    CHECK(engine_lease_wait(lease, 0, err, sizeof err) == ENGINE_WAIT_OK);
}

static engine_state took(harness *h, const char *engine) {
    engine_state st;
    memset(&st, 0, sizeof st);
    CHECK(engine_manager_state(h->manager, engine, &st) == ENGINE_OK);
    return st;
}

static int has_cmd(const harness *h, const char *engine, const char *command) {
    for (int i = 0; i < h->ncmd; i++) {
        if (strcmp(h->cmds[i].engine, engine) == 0 && strcmp(h->cmds[i].command, command) == 0) return 1;
    }
    return 0;
}

static int cmd_is(const harness *h, int index, const char *engine, const char *command) {
    return index >= 0 && index < h->ncmd &&
        strcmp(h->cmds[index].engine, engine) == 0 &&
        strcmp(h->cmds[index].command, command) == 0;
}

static void expect_cmds(const harness *h, int n) {
    if (h->ncmd == n) return;
    fprintf(stderr, "FAIL %s command count %d want %d\n", test_name, h->ncmd, n);
    for (int i = 0; i < h->ncmd; i++) {
        fprintf(stderr, "  %s %s (timeout %.0f)\n", h->cmds[i].engine, h->cmds[i].command, h->cmds[i].timeout);
    }
    failures++;
}

static engine_lease *must_acquire(harness *h, const char *engine) {
    engine_lease *lease = NULL;
    if (engine_manager_acquire(h->manager, engine, &lease) != ENGINE_OK || !lease) {
        CHECK(lease != NULL);
        return NULL;
    }
    return lease;
}

static void test_construction_starts_nothing(void) {
    test_name = "construction";
    harness h;
    setup(&h);
    if (!h.manager) {
        teardown(&h);
        return;
    }
    CHECK(h.ncmd == 0);
    CHECK(h.queue.n == 0);
    CHECK(!took(&h, "stt").running);
    CHECK(!took(&h, "tts").running);
    CHECK(took(&h, "stt").users == 0);
    CHECK(took(&h, "tts").users == 0);
    teardown(&h);
}

static void test_restart_adopts_active_services_with_fresh_idle_grace(void) {
    test_name = "restart_adopts";
    harness h;
    setup(&h);
    h.now = 10000;
    CHECK(engine_manager_reconcile(h.manager, query_stt, &h) == ENGINE_OK);
    CHECK(engine_manager_reconcile(h.manager, query_boom, &h) == ENGINE_OK);
    CHECK(h.nqueries == 0);
    CHECK(!h.boom);
    q_drain(&h.queue);
    CHECK(!h.boom);
    CHECK(h.nqueries == 2);
    CHECK(strcmp(h.queries[0].engine, "stt") == 0);
    CHECK(strcmp(h.queries[1].engine, "tts") == 0);
    CHECK(h.queries[0].timeout == ENGINE_COMMAND_TIMEOUT);
    CHECK(h.queries[1].timeout == ENGINE_COMMAND_TIMEOUT);
    CHECK(took(&h, "stt").running);
    CHECK(!took(&h, "tts").running);
    CHECK(h.ncmd == 0);
    h.now += 899;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    CHECK(h.ncmd == 0);
    h.now += 1;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "stt", "stop"));
    teardown(&h);
}

static void test_retirement_before_startup_adoption_stops_old_service(void) {
    test_name = "retirement_before_adoption";
    harness h;
    setup(&h);
    CHECK(engine_manager_reconcile(h.manager, query_tts, &h) == ENGINE_OK);
    engine_manager_retire(h.manager, "tts");
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "tts", "stop"));
    CHECK(!took(&h, "tts").running);
    teardown(&h);
}

static void test_resident_speech_survives_idle_until_retired(void) {
    test_name = "resident_speech";
    harness h;
    setup(&h);
    engine_lease *lease = NULL;
    CHECK(engine_manager_ensure_resident(h.manager, "tts", &lease) == ENGINE_OK);
    q_drain(&h.queue);
    q_drain(&h.readies);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "tts", "start"));
    h.now += 10000;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    engine_lease *playback = must_acquire(&h, "tts");
    q_drain(&h.queue);
    engine_manager_retire(h.manager, "tts");
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    engine_lease_release(playback);
    q_drain(&h.queue);
    expect_cmds(&h, 2);
    CHECK(cmd_is(&h, 1, "tts", "stop"));
    CHECK(engine_lease_released(lease));
    teardown(&h);
}

typedef struct sys_mock {
    char state[64];
    double timeout;
    int check;
    int argc;
    char argv[8][80];
    int saw_start;
    int fail;
} sys_mock;

static int mock_run(const engine_subprocess_call *call, char *stdout_buf, size_t cap, void *ctx) {
    sys_mock *mock = ctx;
    mock->timeout = call->timeout;
    mock->check = call->check;
    mock->argc = call->argc;
    mock->saw_start = 0;
    for (int i = 0; i < call->argc && i < 8; i++) {
        snprintf(mock->argv[i], sizeof mock->argv[i], "%s", call->argv[i] ? call->argv[i] : "");
        if (strstr(mock->argv[i], "start")) mock->saw_start = 1;
    }
    if (mock->fail) return 1;
    snprintf(stdout_buf, cap, "%s", mock->state);
    return 0;
}

static void test_systemctl_activity_query_is_bounded_and_distinguishes_unknown(void) {
    test_name = "systemctl_activity";
    const char *states[] = {
        "active", "activating", "reloading", "deactivating", "refreshing",
        "inactive", "failed", "", "bogus", "active\n",
    };
    const int resident[] = {1, 1, 1, 1, 1, 0, 0, -1, -1, 1};
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
        sys_mock mock;
        memset(&mock, 0, sizeof mock);
        snprintf(mock.state, sizeof mock.state, "%s", states[i]);
        int active = 0;
        char err[128];
        int rc = engine_systemctl_active("stt", 3, mock_run, &mock, &active, err, sizeof err);
        CHECK(mock.timeout == 3);
        CHECK(mock.check == 1);
        CHECK(!mock.saw_start);
        CHECK(mock.argc == 6);
        CHECK(strcmp(mock.argv[0], "systemctl") == 0);
        CHECK(strcmp(mock.argv[1], "--user") == 0);
        CHECK(strcmp(mock.argv[2], "show") == 0);
        CHECK(strcmp(mock.argv[3], "--property=ActiveState") == 0);
        CHECK(strcmp(mock.argv[4], "--value") == 0);
        CHECK(strcmp(mock.argv[5], "pi-voice-stt.service") == 0);
        if (resident[i] < 0) {
            CHECK(rc != ENGINE_OK);
            CHECK(strstr(err, "unknown ActiveState") != NULL);
        } else {
            CHECK(rc == ENGINE_OK);
            CHECK(active == resident[i]);
        }
    }
    sys_mock denied;
    memset(&denied, 0, sizeof denied);
    denied.fail = 1;
    snprintf(denied.state, sizeof denied.state, "inactive");
    int active = 1;
    char err[128];
    CHECK(engine_systemctl_active("stt", 3, mock_run, &denied, &active, err, sizeof err) != ENGINE_OK);
    CHECK(engine_systemctl("tts", "start", 10, mock_run, &denied, err, sizeof err) != ENGINE_OK);
    denied.fail = 0;
    CHECK(engine_systemctl("tts", "start", 10, mock_run, &denied, err, sizeof err) == ENGINE_OK);
    CHECK(denied.argc == 4);
    CHECK(strcmp(denied.argv[2], "start") == 0);
    CHECK(strcmp(denied.argv[3], "pi-voice-tts.service") == 0);
    CHECK(denied.timeout == 10);
    CHECK(denied.check == 1);
    CHECK(engine_systemctl_active("stt", 3, NULL, NULL, &active, err, sizeof err) != ENGINE_OK);
}

static void test_adoption_query_failure_retains_cleanup_and_admission_fences(void) {
    test_name = "adoption_query_failure";
    harness h;
    setup(&h);
    CHECK(engine_manager_set_population(h.manager, 1, 0) == ENGINE_OK);
    CHECK(engine_manager_reconcile(h.manager, query_unavailable, &h) == ENGINE_OK);
    CHECK(engine_manager_set_population(h.manager, 0, 1) == ENGINE_OK);
    q_drain(&h.queue);
    engine_state st = took(&h, "stt");
    CHECK(st.running);
    CHECK(st.has_error);
    CHECK(h.ncmd == 0);
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    q_drain(&h.queue);
    CHECK(!has_cmd(&h, "stt", "stop"));
    engine_lease_release(lease);
    q_drain(&h.queue);
    CHECK(has_cmd(&h, "stt", "stop"));
    teardown(&h);
}

static void test_nonblocking_engine_specific_and_long_operations(void) {
    test_name = "nonblocking";
    harness h;
    setup(&h);
    engine_lease *lease = must_acquire(&h, "stt");
    CHECK(lease && !engine_lease_done(lease));
    CHECK(h.ncmd == 0);
    become_ready(&h, lease);
    CHECK(h.ncmd == 1);
    CHECK(h.cmds[0].timeout == ENGINE_COMMAND_TIMEOUT);
    CHECK(h.last_ready_timeout == ENGINE_READINESS_TIMEOUT);
    h.now = 2000;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "stt", "start"));
    engine_lease_release(lease);
    CHECK(took(&h, "stt").last_release == 2000);
    CHECK(h.ntimers >= 1);
    CHECK(h.timers[h.ntimers - 1]->delay == ENGINE_IDLE_TIMEOUT);
    h.now = 2899;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    h.now = 2900;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    CHECK(cmd_is(&h, h.ncmd - 1, "stt", "stop"));
    CHECK(h.cmds[h.ncmd - 1].timeout == ENGINE_COMMAND_TIMEOUT);
    teardown(&h);
}

static void test_acquire_invalidates_queued_idle_stop(void) {
    test_name = "acquire_invalidates_stop";
    harness h;
    setup(&h);
    engine_lease *first = must_acquire(&h, "tts");
    become_ready(&h, first);
    engine_lease_release(first);
    h.now = 900;
    engine_manager_sweep(h.manager);
    engine_lease *second = must_acquire(&h, "tts");
    become_ready(&h, second);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "tts", "start"));
    engine_lease_release(second);
    teardown(&h);
}

static void test_stop_in_progress_orders_restart_before_readiness(void) {
    test_name = "stop_in_progress";
    harness h;
    setup(&h);
    h.block_stop = 1;
    engine_lease *first = must_acquire(&h, "stt");
    become_ready(&h, first);
    engine_lease_release(first);
    h.now = 900;
    engine_manager_sweep(h.manager);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, q_run_thread, &h.queue) == 0);
    CHECK(gate_wait(&h.entered, 2000));
    engine_lease *second = must_acquire(&h, "stt");
    CHECK(second && !engine_lease_done(second));
    CHECK(took(&h, "stt").users == 1);
    gate_set(&h.release_gate);
    CHECK(pthread_join(worker, NULL) == 0);
    h.block_stop = 0;
    become_ready(&h, second);
    expect_cmds(&h, 3);
    CHECK(cmd_is(&h, 0, "stt", "start"));
    CHECK(cmd_is(&h, 1, "stt", "stop"));
    CHECK(cmd_is(&h, 2, "stt", "start"));
    engine_lease_release(second);
    teardown(&h);
}

static void test_cancel_loading_and_replace_playback(void) {
    test_name = "cancel_loading";
    harness h;
    setup(&h);
    engine_lease *first = must_acquire(&h, "tts");
    q_drain(&h.queue);
    engine_lease_release(first);
    engine_lease *second = must_acquire(&h, "tts");
    become_ready(&h, second);
    CHECK(engine_lease_cancelled(first));
    CHECK(took(&h, "tts").users == 1);
    h.now = 4000;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    CHECK(!has_cmd(&h, "tts", "stop"));
    engine_lease_release(second);
    engine_lease_release(second);
    CHECK(took(&h, "tts").users == 0);
    teardown(&h);
}

static void test_last_session_waits_for_work_and_pending_admissions(void) {
    test_name = "last_session_waits";
    harness h;
    setup(&h);
    CHECK(engine_manager_set_population(h.manager, 1, 0) == ENGINE_OK);
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    CHECK(engine_manager_set_population(h.manager, 0, 1) == ENGINE_OK);
    engine_lease_release(lease);
    q_drain(&h.queue);
    CHECK(!has_cmd(&h, "stt", "stop"));
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    q_drain(&h.queue);
    CHECK(cmd_is(&h, h.ncmd - 1, "stt", "stop"));
    teardown(&h);
}

static void test_new_registration_fences_zero_session_stop(void) {
    test_name = "new_registration_fences";
    harness h;
    setup(&h);
    CHECK(engine_manager_set_population(h.manager, 1, 0) == ENGINE_OK);
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    engine_lease_release(lease);
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    CHECK(engine_manager_set_population(h.manager, 1, 0) == ENGINE_OK);
    q_drain(&h.queue);
    CHECK(!has_cmd(&h, "stt", "stop"));
    teardown(&h);
}

static void test_initial_zero_and_warm_completion_release_time(void) {
    test_name = "initial_zero_warm";
    harness h;
    setup(&h);
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    engine_manager_sweep(h.manager);
    CHECK(h.queue.n == 0);
    const char *names[] = {"stt"};
    engine_lease *leases[1] = {0};
    size_t n = 0;
    CHECK(engine_manager_warm(h.manager, names, 1, 30, leases, 1, &n) == ENGINE_OK);
    CHECK(n == 1);
    q_drain(&h.queue);
    CHECK(took(&h, "stt").users == 1);
    CHECK(h.ntimers >= 1);
    CHECK(h.timers[h.ntimers - 1]->delay == 30);
    h.now = 25;
    q_drain(&h.readies);
    CHECK(engine_lease_done(leases[0]));
    CHECK(took(&h, "stt").users == 0);
    CHECK(took(&h, "stt").last_release == 25);
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "stt", "start"));
    teardown(&h);
}

static void test_warm_timeout_and_one_backend_failure_release(void) {
    test_name = "warm_timeout";
    harness h;
    setup(&h);
    h.tts_offline = 1;
    const char *names[] = {"stt", "tts"};
    engine_lease *leases[2] = {0};
    size_t n = 0;
    CHECK(engine_manager_warm(h.manager, names, 2, 30, leases, 2, &n) == ENGINE_OK);
    CHECK(n == 2);
    q_drain(&h.queue);
    q_drain(&h.readies);
    char err[256];
    CHECK(engine_lease_wait(leases[0], 0, err, sizeof err) == ENGINE_WAIT_OK);
    int status = engine_lease_wait(leases[1], 0, err, sizeof err);
    CHECK(status == ENGINE_WAIT_FAILED);
    CHECK(strstr(err, "tts") != NULL);
    CHECK(strstr(err, "offline") != NULL);
    CHECK(took(&h, "tts").users == 0);
    CHECK(took(&h, "stt").users == 0);
    h.tts_offline = 0;
    const char *again[] = {"stt"};
    engine_lease *timed[1] = {0};
    CHECK(engine_manager_warm(h.manager, again, 1, 10, timed, 1, &n) == ENGINE_OK);
    test_timer *timer = h.timers[h.ntimers - 1];
    h.now = 10;
    timer_fire(timer);
    status = engine_lease_wait(timed[0], 0, err, sizeof err);
    CHECK(status == ENGINE_WAIT_TIMED_OUT);
    q_drain(&h.queue);
    q_drain(&h.readies);
    CHECK(took(&h, "stt").users == 0);
    teardown(&h);
}

static void test_start_error_and_context_error_release(void) {
    test_name = "start_error";
    harness h;
    setup(&h);
    h.fail_runner = 1;
    snprintf(h.fail_msg, sizeof h.fail_msg, "systemctl failed");
    engine_lease *lease = must_acquire(&h, "stt");
    q_drain(&h.queue);
    char err[256];
    int status = engine_lease_wait(lease, 0, err, sizeof err);
    CHECK(status == ENGINE_WAIT_FAILED);
    CHECK(strstr(err, "stt") != NULL);
    engine_lease_release(lease);
    CHECK(took(&h, "stt").users == 0);
    CHECK(took(&h, "stt").running);
    h.fail_runner = 0;
    engine_lease *playback = must_acquire(&h, "tts");
    become_ready(&h, playback);
    engine_lease_release(playback);
    CHECK(took(&h, "tts").users == 0);
    teardown(&h);
}

static void test_last_session_closes_during_work_and_stops_only_after_release(void) {
    test_name = "last_session_during_work";
    harness h;
    setup(&h);
    CHECK(engine_manager_set_population(h.manager, 1, 0) == ENGINE_OK);
    engine_lease *lease = must_acquire(&h, "tts");
    become_ready(&h, lease);
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    h.now = 2000;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "tts", "start"));
    engine_lease_release(lease);
    q_drain(&h.queue);
    CHECK(cmd_is(&h, h.ncmd - 1, "tts", "stop"));
    teardown(&h);
}

typedef struct waiter {
    engine_lease *lease;
    int status;
    gate started;
    gate finished;
} waiter;

static void *waiter_main(void *arg) {
    waiter *w = arg;
    gate_set(&w->started);
    w->status = engine_lease_wait(w->lease, 2000, NULL, 0);
    gate_set(&w->finished);
    return NULL;
}

static void test_cancel_while_start_command_is_blocked(void) {
    test_name = "cancel_while_start_blocked";
    harness h;
    setup(&h);
    h.block_all = 1;
    CHECK(engine_manager_set_population(h.manager, 1, 0) == ENGINE_OK);
    engine_lease *lease = must_acquire(&h, "stt");
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, q_run_thread, &h.queue) == 0);
    CHECK(gate_wait(&h.entered, 2000));
    waiter wait = {0};
    wait.lease = lease;
    gate_init(&wait.started);
    gate_init(&wait.finished);
    pthread_t waiting;
    CHECK(pthread_create(&waiting, NULL, waiter_main, &wait) == 0);
    CHECK(gate_wait(&wait.started, 2000));
    struct timespec pause = {.tv_nsec = 20000000};
    nanosleep(&pause, NULL);
    engine_lease_release(lease);
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    CHECK(engine_lease_cancelled(lease));
    CHECK(gate_wait(&wait.finished, 2000));
    CHECK(wait.status == ENGINE_WAIT_CANCELLED);
    CHECK(!gate_is_set(&h.release_gate));
    gate_set(&h.release_gate);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(pthread_join(waiting, NULL) == 0);
    h.block_all = 0;
    q_drain(&h.queue);
    CHECK(!took(&h, "stt").running);
    CHECK(took(&h, "stt").users == 0);
    CHECK(h.readies.n == 0);
    gate_destroy(&wait.started);
    gate_destroy(&wait.finished);
    teardown(&h);
}

static void test_pending_admission_invalidates_already_queued_stop(void) {
    test_name = "pending_admission_invalidates";
    harness h;
    setup(&h);
    CHECK(engine_manager_set_population(h.manager, 1, 0) == ENGINE_OK);
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    engine_lease_release(lease);
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    CHECK(engine_manager_set_population(h.manager, 0, 1) == ENGINE_OK);
    q_drain(&h.queue);
    expect_cmds(&h, 1);
    CHECK(cmd_is(&h, 0, "stt", "start"));
    CHECK(engine_manager_set_population(h.manager, 0, 0) == ENGINE_OK);
    q_drain(&h.queue);
    CHECK(cmd_is(&h, h.ncmd - 1, "stt", "stop"));
    teardown(&h);
}

static void test_release_scheduler_error_does_not_mask_operation_error(void) {
    test_name = "release_scheduler_error";
    harness h;
    setup(&h);
    h.fail_later = 1;
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    engine_lease_release(lease);
    CHECK(took(&h, "stt").users == 0);
    CHECK(strstr(took(&h, "stt").error, "timer failed") != NULL);
    h.fail_later = 0;
    h.now = 900;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    CHECK(cmd_is(&h, h.ncmd - 1, "stt", "stop"));
    teardown(&h);
}

typedef struct live {
    gate stt_entered;
    gate stt_release;
    gate tts_started;
    pthread_mutex_t mu;
    int ncalls;
    char engines[4][8];
    char commands[4][16];
    double timeout;
} live;

static int live_runner(
    const char *engine, const char *command, double timeout, void *ctx,
    char *err, size_t err_cap
) {
    live *box = ctx;
    (void)err;
    (void)err_cap;
    pthread_mutex_lock(&box->mu);
    if (box->ncalls < 4) {
        snprintf(box->engines[box->ncalls], sizeof box->engines[0], "%s", engine);
        snprintf(box->commands[box->ncalls], sizeof box->commands[0], "%s", command);
        box->ncalls++;
    }
    box->timeout = timeout;
    int tts = strcmp(engine, "tts") == 0;
    pthread_mutex_unlock(&box->mu);
    if (tts) gate_set(&box->tts_started);
    return 0;
}

static int live_ready(const char *engine, double timeout, void *ctx, char *err, size_t err_cap) {
    live *box = ctx;
    (void)timeout;
    if (strcmp(engine, "stt") == 0) {
        gate_set(&box->stt_entered);
        if (!gate_wait(&box->stt_release, 2000)) {
            snprintf(err, err_cap, "test barrier");
            return -1;
        }
    }
    return 1;
}

static void test_default_worker_serializes_commands_while_readiness_blocks(void) {
    test_name = "default_worker";
    live box;
    memset(&box, 0, sizeof box);
    gate_init(&box.stt_entered);
    gate_init(&box.stt_release);
    gate_init(&box.tts_started);
    pthread_mutex_init(&box.mu, NULL);
    engine_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.runner = live_runner;
    cfg.runner_ctx = &box;
    cfg.readiness = live_ready;
    cfg.readiness_ctx = &box;
    engine_manager *manager = engine_manager_create(&cfg);
    CHECK(manager != NULL);
    engine_lease *stt = NULL;
    engine_lease *tts = NULL;
    if (manager && engine_manager_acquire(manager, "stt", &stt) == ENGINE_OK) {
        CHECK(gate_wait(&box.stt_entered, 2000));
        engine_state st;
        CHECK(engine_manager_state(manager, "stt", &st) == ENGINE_OK);
        CHECK(st.users == 1);
        CHECK(engine_manager_acquire(manager, "tts", &tts) == ENGINE_OK);
        CHECK(gate_wait(&box.tts_started, 2000));
        char err[128];
        CHECK(engine_lease_wait(tts, 2000, err, sizeof err) == ENGINE_WAIT_OK);
        engine_lease_release(stt);
        CHECK(engine_lease_cancelled(stt));
        engine_lease_release(tts);
    }
    gate_set(&box.stt_release);
    engine_manager_close(manager);
    engine_manager_free(manager);
    CHECK(box.ncalls == 2);
    CHECK(strcmp(box.engines[0], "stt") == 0 && strcmp(box.commands[0], "start") == 0);
    CHECK(strcmp(box.engines[1], "tts") == 0 && strcmp(box.commands[1], "start") == 0);
    CHECK(box.timeout == ENGINE_COMMAND_TIMEOUT);
    gate_destroy(&box.stt_entered);
    gate_destroy(&box.stt_release);
    gate_destroy(&box.tts_started);
    pthread_mutex_destroy(&box.mu);
}

static void test_stop_error_forces_restart_for_next_caller(void) {
    test_name = "stop_error_restarts";
    harness h;
    setup(&h);
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    engine_lease_release(lease);
    h.now = 900;
    h.fail_runner = 1;
    snprintf(h.fail_msg, sizeof h.fail_msg, "partial stop");
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    h.fail_runner = 0;
    engine_lease *second = must_acquire(&h, "stt");
    become_ready(&h, second);
    expect_cmds(&h, 2);
    CHECK(cmd_is(&h, 0, "stt", "start"));
    CHECK(cmd_is(&h, 1, "stt", "start"));
    engine_lease_release(second);
    teardown(&h);
}

static void test_stop_error_keeps_retryable_state(void) {
    test_name = "stop_error_retryable";
    harness h;
    setup(&h);
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    engine_lease_release(lease);
    h.now = 900;
    h.fail_runner = 1;
    snprintf(h.fail_msg, sizeof h.fail_msg, "stop failed");
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    CHECK(strstr(took(&h, "stt").error, "stop failed") != NULL);
    h.fail_runner = 0;
    engine_manager_sweep(h.manager);
    q_drain(&h.queue);
    CHECK(!took(&h, "stt").running);
    CHECK(has_cmd(&h, "stt", "stop"));
    teardown(&h);
}

static void test_warm_all_releases_on_readiness(void) {
    test_name = "warm_all";
    harness h;
    setup(&h);
    engine_lease *leases[2] = {0};
    size_t n = 0;
    CHECK(engine_manager_warm(h.manager, NULL, 0, 30, leases, 2, &n) == ENGINE_OK);
    CHECK(n == 2);
    q_drain(&h.queue);
    CHECK(took(&h, "stt").users == 1);
    CHECK(took(&h, "tts").users == 1);
    CHECK(has_cmd(&h, "stt", "start"));
    CHECK(has_cmd(&h, "tts", "start"));
    q_drain(&h.readies);
    CHECK(took(&h, "stt").users == 0);
    CHECK(took(&h, "tts").users == 0);
    CHECK(engine_lease_done(leases[0]));
    CHECK(engine_lease_done(leases[1]));
    teardown(&h);
}

static void test_close_cancels_leases_without_stopping_units(void) {
    test_name = "close_without_stop";
    harness h;
    setup(&h);
    engine_lease *lease = must_acquire(&h, "stt");
    become_ready(&h, lease);
    engine_manager_close(h.manager);
    q_drain(&h.queue);
    CHECK(has_cmd(&h, "stt", "start"));
    CHECK(!has_cmd(&h, "stt", "stop"));
    CHECK(engine_lease_cancelled(lease) || engine_lease_done(lease));
    teardown(&h);
}

static long vm_size_kb(void) {
    FILE *file = fopen("/proc/self/status", "r");
    char line[256];
    long size = -1;
    if (!file) return -1;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "VmSize: %ld", &size) == 1) break;
    }
    fclose(file);
    return size;
}

static int reap_runner(
    const char *engine, const char *command, double timeout, void *ctx,
    char *err, size_t err_cap
) {
    (void)engine;
    (void)command;
    (void)timeout;
    (void)ctx;
    (void)err;
    (void)err_cap;
    return 0;
}

static int reap_ready(const char *engine, double timeout, void *ctx, char *err, size_t err_cap) {
    (void)engine;
    (void)timeout;
    (void)ctx;
    (void)err;
    (void)err_cap;
    return 1;
}

static void test_default_timers_do_not_accumulate(void) {
    test_name = "default_timer_reap";
    engine_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.runner = reap_runner;
    cfg.readiness = reap_ready;
    cfg.idle_timeout = 3600;
    const char *names[] = {"stt"};
    cfg.engines = names;
    cfg.engine_count = 1;
    engine_manager *manager = engine_manager_create(&cfg);
    CHECK(manager != NULL);
    if (!manager) return;
    engine_lease *warm = NULL;
    char err[128];
    CHECK(engine_manager_acquire(manager, "stt", &warm) == ENGINE_OK);
    CHECK(engine_lease_wait(warm, 2000, err, sizeof err) == ENGINE_WAIT_OK);
    engine_lease_release(warm);
    CHECK(engine_lease_released(warm));
    long before = vm_size_kb();
    for (int i = 0; i < 12; i++) {
        engine_lease *lease = NULL;
        CHECK(engine_manager_acquire(manager, "stt", &lease) == ENGINE_OK);
        CHECK(engine_lease_wait(lease, 2000, err, sizeof err) == ENGINE_WAIT_OK);
        engine_lease_release(lease);
        CHECK(engine_lease_released(lease));
    }
    long after = vm_size_kb();
    /* Cancellation is nonblocking: let callbacks exit, then trigger reaping.
       Measuring immediately after a burst counts still-running threads as leaks.
       The allowance includes libc's stack cache and sanitizer metadata. */
    for (int retry = 0; after > before + 49152 && retry < 200; retry++) {
        struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
        engine_lease *lease = NULL;
        CHECK(engine_manager_acquire(manager, "stt", &lease) == ENGINE_OK);
        CHECK(engine_lease_wait(lease, 2000, err, sizeof err) == ENGINE_WAIT_OK);
        engine_lease_release(lease);
        after = vm_size_kb();
    }
    CHECK(before > 0 && after > 0);
    if (after > before + 49152) fprintf(stderr, "timer VmSize: %ld -> %ld KiB\n", before, after);
    CHECK(after <= before + 49152);
    engine_manager_free(manager);
}

static void test_negative_population_is_rejected(void) {
    test_name = "negative_population";
    harness h;
    setup(&h);
    CHECK(engine_manager_set_population(h.manager, -1, 0) == ENGINE_INVALID);
    CHECK(engine_manager_set_population(h.manager, 0, -1) == ENGINE_INVALID);
    CHECK(engine_manager_warm(h.manager, NULL, 0, 0, (engine_lease *[1]){0}, 1, &(size_t){0}) == ENGINE_INVALID);
    teardown(&h);
}

int test_engines(void) {
    failures = 0;
    test_construction_starts_nothing();
    test_restart_adopts_active_services_with_fresh_idle_grace();
    test_retirement_before_startup_adoption_stops_old_service();
    test_resident_speech_survives_idle_until_retired();
    test_systemctl_activity_query_is_bounded_and_distinguishes_unknown();
    test_adoption_query_failure_retains_cleanup_and_admission_fences();
    test_nonblocking_engine_specific_and_long_operations();
    test_acquire_invalidates_queued_idle_stop();
    test_stop_in_progress_orders_restart_before_readiness();
    test_cancel_loading_and_replace_playback();
    test_last_session_waits_for_work_and_pending_admissions();
    test_new_registration_fences_zero_session_stop();
    test_initial_zero_and_warm_completion_release_time();
    test_warm_timeout_and_one_backend_failure_release();
    test_start_error_and_context_error_release();
    test_last_session_closes_during_work_and_stops_only_after_release();
    test_cancel_while_start_command_is_blocked();
    test_pending_admission_invalidates_already_queued_stop();
    test_release_scheduler_error_does_not_mask_operation_error();
    test_default_worker_serializes_commands_while_readiness_blocks();
    test_stop_error_forces_restart_for_next_caller();
    test_stop_error_keeps_retryable_state();
    test_warm_all_releases_on_readiness();
    test_close_cancels_leases_without_stopping_units();
    test_default_timers_do_not_accumulate();
    test_negative_population_is_rejected();
    return failures;
}

#ifdef TEST_ENGINES_MAIN
int main(void) {
    int failed = test_engines();
    if (failed) {
        fprintf(stderr, "%d failure%s\n", failed, failed == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
#endif
