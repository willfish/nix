#ifndef PI_VOICE_ENGINES_H
#define PI_VOICE_ENGINES_H

#include <stddef.h>

/* Serialized stt/tts unit leases. Construction starts nothing.
 * systemctl and the clock are injected; this module never execs systemctl.
 * Idle timeout is measured from release. acquire does not wait for a command.
 */

#define ENGINE_IDLE_TIMEOUT 900.0
#define ENGINE_READINESS_TIMEOUT 60.0
#define ENGINE_COMMAND_TIMEOUT 10.0

enum {
    ENGINE_OK = 0,
    ENGINE_ERR = -1,
    ENGINE_INVALID = -2,
    ENGINE_CLOSED = -3
};

enum engine_wait_status {
    ENGINE_WAIT_OK = 0,
    ENGINE_WAIT_UNREADY = 1,
    ENGINE_WAIT_CANCELLED = 2,
    ENGINE_WAIT_FAILED = 3,
    ENGINE_WAIT_TIMED_OUT = 4
};

typedef struct engine_manager engine_manager;
typedef struct engine_lease engine_lease;
typedef struct engine_timer engine_timer;

struct engine_timer {
    void (*cancel)(engine_timer *timer);
};

typedef struct engine_state {
    int users;
    int generation;
    double last_release;
    char command[16];
    int running;
    int has_error;
    char error[512];
    int retire_when_idle;
} engine_state;

/* runner: 0 started/stopped, nonzero failure with err set. */
typedef int (*engine_runner_fn)(
    const char *engine, const char *command, double timeout, void *ctx,
    char *err, size_t err_cap);

/* 1 ready, 0 timeout, negative failure with err set. */
typedef int (*engine_readiness_fn)(
    const char *engine, double timeout, void *ctx, char *err, size_t err_cap);

typedef double (*engine_clock_fn)(void *ctx);

/* 0 accepted ownership of arg. Nonzero: caller still owns arg. Must not run inline. */
typedef int (*engine_submit_fn)(void (*fn)(void *), void *arg, void *ctx);

/* 0 and *out set, or nonzero with err set. */
typedef int (*engine_call_later_fn)(
    double delay, void (*fn)(void *), void *arg, void *ctx,
    engine_timer **out, char *err, size_t err_cap);

/* 0 and *active 0/1. Nonzero, or *active not 0/1, is not definitive. */
typedef int (*engine_activity_fn)(
    const char *engine, double timeout, void *ctx, int *active,
    char *err, size_t err_cap);

typedef struct engine_config {
    engine_runner_fn runner;
    void *runner_ctx;
    engine_readiness_fn readiness;
    void *readiness_ctx;
    engine_clock_fn clock;
    void *clock_ctx;
    engine_submit_fn submit_command; /* NULL: one FIFO worker */
    void *submit_command_ctx;
    engine_submit_fn submit_readiness; /* NULL: readiness readers */
    void *submit_readiness_ctx;
    engine_call_later_fn call_later; /* NULL: internal timers */
    void *call_later_ctx;
    double idle_timeout; /* 0 selects 900 */
    double readiness_timeout; /* 0 selects 60 */
    double command_timeout; /* 0 selects 10 */
    const char *const *engines; /* NULL selects stt, tts */
    size_t engine_count;
} engine_config;

engine_manager *engine_manager_create(const engine_config *cfg);
void engine_manager_close(engine_manager *manager);
void engine_manager_free(engine_manager *manager);

int engine_manager_reconcile(engine_manager *manager, engine_activity_fn query, void *ctx);
int engine_manager_ensure_resident(engine_manager *manager, const char *engine, engine_lease **out);
void engine_manager_retire(engine_manager *manager, const char *engine);
int engine_manager_acquire(engine_manager *manager, const char *engine, engine_lease **out);
int engine_manager_warm(
    engine_manager *manager, const char *const *engines, size_t count, double timeout,
    engine_lease **out, size_t cap, size_t *out_count);
int engine_manager_set_population(engine_manager *manager, int sessions, int pending_admissions);
void engine_manager_sweep(engine_manager *manager);
int engine_manager_state(engine_manager *manager, const char *engine, engine_state *out);

const char *engine_lease_name(const engine_lease *lease);
int engine_lease_done(engine_lease *lease);
int engine_lease_cancelled(engine_lease *lease);
int engine_lease_released(engine_lease *lease);
/* timeout_ms < 0 waits, 0 polls, > 0 waits that many milliseconds. */
int engine_lease_wait(engine_lease *lease, int timeout_ms, char *err, size_t err_cap);
void engine_lease_release(engine_lease *lease);

typedef struct engine_subprocess_call {
    const char *argv[8];
    int argc;
    double timeout;
    int check;
} engine_subprocess_call;

/* 0 and stdout_buf filled, or nonzero. Must not exec unless ctx does. */
typedef int (*engine_subprocess_fn)(
    const engine_subprocess_call *call, char *stdout_buf, size_t stdout_cap, void *ctx);

int engine_active_from_text(
    const char *engine, const char *text, int *resident, char *err, size_t err_cap);
int engine_systemctl_active(
    const char *engine, double timeout, engine_subprocess_fn run, void *ctx,
    int *resident, char *err, size_t err_cap);
int engine_systemctl(
    const char *engine, const char *command, double timeout,
    engine_subprocess_fn run, void *ctx, char *err, size_t err_cap);

#endif
