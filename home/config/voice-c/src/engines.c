#define _POSIX_C_SOURCE 200809L
#include "engines.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ENGINE_SLOTS = 2, STOP_TICKETS = 8, LEASE_CALLBACKS = 4, READER_THREADS = 4 };

typedef void (*lease_cb)(engine_lease *lease, void *ctx);

struct engine_lease {
    engine_manager *manager;
    char engine[8];
    int released;
    int ready;
    int status;
    char error[512];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    engine_timer *timer;
    lease_cb cbs[LEASE_CALLBACKS];
    void *cb_ctx[LEASE_CALLBACKS];
    int ncb;
    int refs;
    void *timer_hold;
    struct engine_lease *next;
    struct engine_lease *all_next;
};

typedef struct engine_slot {
    char name[8];
    int users;
    int generation;
    double last_release;
    char command[16];
    int running;
    int has_error;
    char error[512];
    int retire_when_idle;
    engine_timer *idle_timer;
    engine_lease *resident;
} engine_slot;

typedef struct stop_ticket {
    int used;
    char engine[8];
    int generation;
    int population;
} stop_ticket;

typedef struct job {
    struct job *next;
    void (*fn)(void *);
    void (*discard)(void *);
    void *arg;
} job;

typedef struct executor {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    job *head;
    job *tail;
    pthread_t threads[READER_THREADS];
    int capacity;
    int nthreads;
    int started;
    int stopping;
} executor;

typedef struct default_timer {
    engine_timer base;
    struct default_timer *next;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int cancelled;
    int started;
    int finished;
    double delay;
    void (*fn)(void *);
    void *arg;
    engine_manager *manager;
} default_timer;

struct engine_manager {
    pthread_mutex_t lock;
    engine_runner_fn runner;
    void *runner_ctx;
    engine_readiness_fn readiness;
    void *readiness_ctx;
    engine_clock_fn clock;
    void *clock_ctx;
    engine_submit_fn submit_command;
    void *submit_command_ctx;
    engine_submit_fn submit_readiness;
    void *submit_readiness_ctx;
    engine_call_later_fn call_later;
    void *call_later_ctx;
    double idle_timeout;
    double readiness_timeout;
    double command_timeout;
    executor commands;
    executor readers;
    int own_commands;
    int own_readers;
    engine_slot slots[ENGINE_SLOTS];
    int nengines;
    engine_lease *leases;
    engine_lease *all_leases;
    stop_ticket stops[STOP_TICKETS];
    default_timer *timers;
    int sessions;
    int admissions;
    int population_generation;
    int early_stop;
    int closed;
    int reconciled;
    int shutdown;
};

static void sweep_locked(engine_manager *manager);
static void complete_lease(engine_manager *manager, engine_lease *lease, int status, const char *err);

static int valid_engine_name(const char *name) {
    return name && (strcmp(name, "stt") == 0 || strcmp(name, "tts") == 0);
}

static double clock_of(engine_manager *manager) {
    if (manager->clock) return manager->clock(manager->clock_ctx);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void add_ms(struct timespec *ts, int ms) {
    if (ms < 0) ms = 0;
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static void add_seconds(struct timespec *ts, double seconds) {
    if (seconds < 0) seconds = 0;
    time_t whole = (time_t)seconds;
    double fraction = seconds - (double)whole;
    ts->tv_sec += whole;
    ts->tv_nsec += (long)(fraction * 1000000000.0);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static engine_slot *find_slot(engine_manager *manager, const char *engine) {
    if (!engine) return NULL;
    for (int i = 0; i < manager->nengines; i++) {
        if (strcmp(manager->slots[i].name, engine) == 0) return &manager->slots[i];
    }
    return NULL;
}

static void set_error(engine_slot *slot, const char *message) {
    slot->has_error = 1;
    snprintf(slot->error, sizeof slot->error, "%s", message ? message : "");
}

static void clear_error(engine_slot *slot) {
    slot->has_error = 0;
    slot->error[0] = '\0';
}

static void copy_state(const engine_slot *slot, engine_state *out) {
    memset(out, 0, sizeof *out);
    out->users = slot->users;
    out->generation = slot->generation;
    out->last_release = slot->last_release;
    snprintf(out->command, sizeof out->command, "%s", slot->command);
    out->running = slot->running;
    out->has_error = slot->has_error;
    snprintf(out->error, sizeof out->error, "%s", slot->error);
    out->retire_when_idle = slot->retire_when_idle;
}

static int executor_init(executor *executor, int capacity) {
    memset(executor, 0, sizeof *executor);
    executor->capacity = capacity;
    if (pthread_mutex_init(&executor->mu, NULL) != 0) return -1;
    if (pthread_cond_init(&executor->cv, NULL) != 0) {
        pthread_mutex_destroy(&executor->mu);
        return -1;
    }
    return 0;
}

static void executor_destroy(executor *executor) {
    pthread_cond_destroy(&executor->cv);
    pthread_mutex_destroy(&executor->mu);
}

static void *executor_main(void *arg) {
    executor *executor = arg;
    for (;;) {
        pthread_mutex_lock(&executor->mu);
        while (!executor->head && !executor->stopping) {
            pthread_cond_wait(&executor->cv, &executor->mu);
        }
        if (!executor->head && executor->stopping) {
            pthread_mutex_unlock(&executor->mu);
            break;
        }
        job *next = executor->head;
        executor->head = next->next;
        if (!executor->head) executor->tail = NULL;
        pthread_mutex_unlock(&executor->mu);
        next->fn(next->arg);
        free(next);
    }
    return NULL;
}

static int executor_submit_owned(void (*fn)(void *), void (*discard)(void *), void *arg, void *ctx) {
    executor *executor = ctx;
    pthread_mutex_lock(&executor->mu);
    if (executor->stopping) {
        pthread_mutex_unlock(&executor->mu);
        return -1;
    }
    if (!executor->started) {
        for (int i = 0; i < executor->capacity; i++) {
            if (pthread_create(&executor->threads[i], NULL, executor_main, executor) != 0) {
                executor->stopping = 1;
                pthread_cond_broadcast(&executor->cv);
                int started = executor->nthreads;
                pthread_mutex_unlock(&executor->mu);
                for (int j = 0; j < started; j++) pthread_join(executor->threads[j], NULL);
                pthread_mutex_lock(&executor->mu);
                executor->nthreads = 0;
                executor->started = 0;
                executor->stopping = 0;
                pthread_mutex_unlock(&executor->mu);
                return -1;
            }
            executor->nthreads++;
        }
        executor->started = 1;
    }
    job *next = calloc(1, sizeof *next);
    if (!next) {
        pthread_mutex_unlock(&executor->mu);
        return -1;
    }
    next->fn = fn;
    next->discard = discard;
    next->arg = arg;
    if (executor->tail) executor->tail->next = next;
    else executor->head = next;
    executor->tail = next;
    pthread_cond_signal(&executor->cv);
    pthread_mutex_unlock(&executor->mu);
    return 0;
}

static int executor_submit(void (*fn)(void *), void *arg, void *ctx) {
    return executor_submit_owned(fn, NULL, arg, ctx);
}

static void executor_shutdown(executor *executor) {
    pthread_mutex_lock(&executor->mu);
    executor->stopping = 1;
    job *pending = executor->head;
    executor->head = NULL;
    executor->tail = NULL;
    pthread_cond_broadcast(&executor->cv);
    int nthreads = executor->nthreads;
    pthread_t threads[READER_THREADS];
    for (int i = 0; i < nthreads; i++) threads[i] = executor->threads[i];
    pthread_mutex_unlock(&executor->mu);
    while (pending) {
        job *next = pending->next;
        if (pending->discard) pending->discard(pending->arg);
        else free(pending->arg);
        free(pending);
        pending = next;
    }
    for (int i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);
    pthread_mutex_lock(&executor->mu);
    executor->nthreads = 0;
    executor->started = 0;
    pthread_mutex_unlock(&executor->mu);
}

/* May run with the manager lock held. Join only callbacks that have already returned. */
static void reap_finished_timers(engine_manager *manager) {
    default_timer *doomed = NULL;
    pthread_mutex_lock(&manager->lock);
    default_timer **link = &manager->timers;
    while (*link) {
        default_timer *item = *link;
        int claim = 0;
        if (item->started && !pthread_equal(item->thread, pthread_self())) {
            pthread_mutex_lock(&item->mu);
            claim = item->cancelled && item->finished;
            pthread_mutex_unlock(&item->mu);
        }
        if (claim) {
            *link = item->next;
            item->next = doomed;
            doomed = item;
        } else {
            link = &item->next;
        }
    }
    pthread_mutex_unlock(&manager->lock);
    while (doomed) {
        default_timer *next = doomed->next;
        pthread_join(doomed->thread, NULL);
        pthread_cond_destroy(&doomed->cv);
        pthread_mutex_destroy(&doomed->mu);
        free(doomed);
        doomed = next;
    }
}

static void default_timer_cancel(engine_timer *timer) {
    default_timer *item = (default_timer *)timer;
    pthread_mutex_lock(&item->mu);
    item->cancelled = 1;
    pthread_cond_signal(&item->cv);
    pthread_mutex_unlock(&item->mu);
    if (item->manager) reap_finished_timers(item->manager);
}

static void *default_timer_main(void *arg) {
    default_timer *item = arg;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    add_seconds(&ts, item->delay);
    pthread_mutex_lock(&item->mu);
    while (!item->cancelled) {
        int rc = pthread_cond_timedwait(&item->cv, &item->mu, &ts);
        if (rc == ETIMEDOUT) break;
    }
    int fire = !item->cancelled;
    pthread_mutex_unlock(&item->mu);
    if (fire && item->fn) item->fn(item->arg);
    pthread_mutex_lock(&item->mu);
    item->finished = 1;
    pthread_mutex_unlock(&item->mu);
    return NULL;
}

static int default_call_later(
    double delay, void (*fn)(void *), void *arg, void *ctx,
    engine_timer **out, char *err, size_t err_cap
) {
    engine_manager *manager = ctx;
    default_timer *item = calloc(1, sizeof *item);
    if (!item) {
        if (err && err_cap) snprintf(err, err_cap, "out of memory");
        return -1;
    }
    item->base.cancel = default_timer_cancel;
    item->manager = manager;
    item->delay = delay;
    item->fn = fn;
    item->arg = arg;
    reap_finished_timers(manager);
    pthread_mutex_init(&item->mu, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&item->cv, &attr);
    pthread_condattr_destroy(&attr);
    if (pthread_create(&item->thread, NULL, default_timer_main, item) != 0) {
        pthread_cond_destroy(&item->cv);
        pthread_mutex_destroy(&item->mu);
        free(item);
        if (err && err_cap) snprintf(err, err_cap, "timer failed");
        return -1;
    }
    item->started = 1;
    pthread_mutex_lock(&manager->lock);
    item->next = manager->timers;
    manager->timers = item;
    pthread_mutex_unlock(&manager->lock);
    *out = &item->base;
    return 0;
}

static void join_timers(engine_manager *manager) {
    pthread_mutex_lock(&manager->lock);
    for (default_timer *item = manager->timers; item; item = item->next) {
        pthread_mutex_lock(&item->mu);
        item->cancelled = 1;
        pthread_cond_signal(&item->cv);
        pthread_mutex_unlock(&item->mu);
    }
    default_timer *items = manager->timers;
    manager->timers = NULL;
    pthread_mutex_unlock(&manager->lock);
    while (items) {
        default_timer *next = items->next;
        if (items->started) pthread_join(items->thread, NULL);
        pthread_cond_destroy(&items->cv);
        pthread_mutex_destroy(&items->mu);
        free(items);
        items = next;
    }
}

static void lease_finish_locked_snapshot(
    engine_lease *lease, int status, const char *err,
    lease_cb *cbs, void **ctxs, int *ncb
) {
    lease->ready = 1;
    lease->status = status;
    snprintf(lease->error, sizeof lease->error, "%s", err ? err : "");
    *ncb = lease->ncb;
    for (int i = 0; i < *ncb; i++) {
        cbs[i] = lease->cbs[i];
        ctxs[i] = lease->cb_ctx[i];
    }
    pthread_cond_broadcast(&lease->cv);
}

static void run_callbacks(engine_lease *lease, lease_cb *cbs, void **ctxs, int ncb) {
    for (int i = 0; i < ncb; i++) {
        if (cbs[i]) cbs[i](lease, ctxs[i]);
    }
}

static void cancel_ready(engine_lease *lease) {
    lease_cb cbs[LEASE_CALLBACKS];
    void *ctxs[LEASE_CALLBACKS];
    int ncb = 0;
    pthread_mutex_lock(&lease->mu);
    if (!lease->ready) {
        lease_finish_locked_snapshot(lease, ENGINE_WAIT_CANCELLED, NULL, cbs, ctxs, &ncb);
    }
    pthread_mutex_unlock(&lease->mu);
    run_callbacks(lease, cbs, ctxs, ncb);
}

static void complete_lease(engine_manager *manager, engine_lease *lease, int status, const char *err) {
    lease_cb cbs[LEASE_CALLBACKS];
    void *ctxs[LEASE_CALLBACKS];
    int ncb = 0;
    pthread_mutex_lock(&manager->lock);
    pthread_mutex_lock(&lease->mu);
    if (lease->released || lease->ready) {
        pthread_mutex_unlock(&lease->mu);
        pthread_mutex_unlock(&manager->lock);
        return;
    }
    if (status != ENGINE_WAIT_OK) {
        engine_slot *slot = find_slot(manager, lease->engine);
        if (slot) set_error(slot, err ? err : "");
    }
    lease_finish_locked_snapshot(lease, status, err, cbs, ctxs, &ncb);
    pthread_mutex_unlock(&lease->mu);
    run_callbacks(lease, cbs, ctxs, ncb);
    pthread_mutex_unlock(&manager->lock);
}

static int lease_is_ready(engine_lease *lease) {
    pthread_mutex_lock(&lease->mu);
    int ready = lease->ready;
    pthread_mutex_unlock(&lease->mu);
    return ready;
}

static void lease_on_done(engine_lease *lease, lease_cb cb, void *ctx) {
    int run = 0;
    pthread_mutex_lock(&lease->mu);
    if (lease->ready) run = 1;
    else if (lease->ncb < LEASE_CALLBACKS) {
        lease->cbs[lease->ncb] = cb;
        lease->cb_ctx[lease->ncb] = ctx;
        lease->ncb++;
    }
    pthread_mutex_unlock(&lease->mu);
    if (run && cb) cb(lease, ctx);
}

static int eligible(engine_manager *manager, const engine_slot *slot) {
    if (manager->closed || slot->users != 0 || !slot->running || manager->admissions) return 0;
    if (manager->early_stop) return 1;
    return clock_of(manager) - slot->last_release >= manager->idle_timeout;
}

static int ticket_index(const engine_manager *manager, const char *engine, int generation, int population) {
    for (int i = 0; i < STOP_TICKETS; i++) {
        const stop_ticket *ticket = &manager->stops[i];
        if (ticket->used && ticket->generation == generation && ticket->population == population &&
            strcmp(ticket->engine, engine) == 0) {
            return i;
        }
    }
    return -1;
}

static void discard_ticket(engine_manager *manager, const char *engine, int generation, int population) {
    int index = ticket_index(manager, engine, generation, population);
    if (index >= 0) manager->stops[index].used = 0;
}

struct stop_arg {
    engine_manager *manager;
    char engine[8];
    int generation;
    int population;
};

static void stop_job(void *arg) {
    struct stop_arg *stop = arg;
    engine_manager *manager = stop->manager;
    pthread_mutex_lock(&manager->lock);
    discard_ticket(manager, stop->engine, stop->generation, stop->population);
    engine_slot *slot = find_slot(manager, stop->engine);
    if (!slot || slot->generation != stop->generation ||
        manager->population_generation != stop->population || !eligible(manager, slot)) {
        pthread_mutex_unlock(&manager->lock);
        free(stop);
        return;
    }
    slot->running = 0;
    snprintf(slot->command, sizeof slot->command, "stop");
    engine_runner_fn runner = manager->runner;
    void *runner_ctx = manager->runner_ctx;
    double timeout = manager->command_timeout;
    char engine[8];
    snprintf(engine, sizeof engine, "%s", stop->engine);
    pthread_mutex_unlock(&manager->lock);

    char detail[256];
    detail[0] = '\0';
    int failed = !runner || runner(engine, "stop", timeout, runner_ctx, detail, sizeof detail) != 0;
    pthread_mutex_lock(&manager->lock);
    slot = find_slot(manager, engine);
    if (slot) {
        if (failed) {
            char message[512];
            snprintf(message, sizeof message, "%s: %s", engine, detail[0] ? detail : "command failed");
            slot->running = 1;
            set_error(slot, message);
        } else {
            clear_error(slot);
        }
        slot->command[0] = '\0';
    }
    pthread_mutex_unlock(&manager->lock);
    free(stop);
}

static void sweep_locked(engine_manager *manager) {
    for (int i = 0; i < manager->nengines; i++) {
        engine_slot *slot = &manager->slots[i];
        if (!eligible(manager, slot)) continue;
        if (ticket_index(manager, slot->name, slot->generation, manager->population_generation) >= 0) {
            continue;
        }
        int free_index = -1;
        for (int t = 0; t < STOP_TICKETS; t++) {
            if (!manager->stops[t].used) {
                free_index = t;
                break;
            }
        }
        if (free_index < 0) {
            set_error(slot, "stop queue full");
            continue;
        }
        stop_ticket *ticket = &manager->stops[free_index];
        ticket->used = 1;
        ticket->generation = slot->generation;
        ticket->population = manager->population_generation;
        snprintf(ticket->engine, sizeof ticket->engine, "%s", slot->name);
        struct stop_arg *stop = calloc(1, sizeof *stop);
        if (!stop) {
            ticket->used = 0;
            set_error(slot, "out of memory");
            continue;
        }
        stop->manager = manager;
        stop->generation = ticket->generation;
        stop->population = ticket->population;
        snprintf(stop->engine, sizeof stop->engine, "%s", slot->name);
        if (manager->submit_command(stop_job, stop, manager->submit_command_ctx) != 0) {
            ticket->used = 0;
            free(stop);
            char message[128];
            snprintf(message, sizeof message, "%s: submit failed", slot->name);
            set_error(slot, message);
        }
    }
}

static void sweep_thunk(void *arg) {
    engine_manager_sweep(arg);
}

static void unlink_all(engine_manager *manager, engine_lease *lease) {
    engine_lease **link = &manager->all_leases;
    while (*link) {
        if (*link == lease) {
            *link = lease->all_next;
            lease->all_next = NULL;
            return;
        }
        link = &(*link)->all_next;
    }
}

static void destroy_lease(engine_lease *lease) {
    pthread_cond_destroy(&lease->cv);
    pthread_mutex_destroy(&lease->mu);
    free(lease);
}

void engine_lease_ref(engine_lease *lease) {
    if (!lease) return;
    pthread_mutex_lock(&lease->manager->lock);
    lease->refs++;
    pthread_mutex_unlock(&lease->manager->lock);
}

void engine_lease_unref(engine_lease *lease) {
    if (!lease) return;
    engine_manager *manager = lease->manager;
    int free_now = 0;
    pthread_mutex_lock(&manager->lock);
    if (lease->refs > 0) lease->refs--;
    free_now = lease->refs == 0;
    if (free_now && !lease->released) {
        lease->refs = 1;
        pthread_mutex_unlock(&manager->lock);
        engine_lease_release(lease);
        pthread_mutex_lock(&manager->lock);
        if (lease->refs > 0) lease->refs--;
        free_now = lease->refs == 0;
    }
    if (free_now) unlink_all(manager, lease);
    pthread_mutex_unlock(&manager->lock);
    if (free_now) destroy_lease(lease);
}

int engine_manager_live_leases(engine_manager *manager) {
    if (!manager) return 0;
    pthread_mutex_lock(&manager->lock);
    int count = 0;
    for (engine_lease *lease = manager->all_leases; lease; lease = lease->all_next) count++;
    pthread_mutex_unlock(&manager->lock);
    return count;
}

typedef struct lease_hold {
    engine_lease *lease;
    void (*fn)(void *);
    void *arg;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int claimed;
    int in_fn;
    int dropped;
    int fire_frees;
} lease_hold;

static __thread int hold_fire_depth;

static void hold_fire(void *arg) {
    lease_hold *hold = arg;
    hold_fire_depth++;
    pthread_mutex_lock(&hold->mu);
    if (hold->dropped) {
        pthread_mutex_unlock(&hold->mu);
        hold_fire_depth--;
        return;
    }
    hold->in_fn = 1;
    pthread_mutex_unlock(&hold->mu);
    if (hold->fn) hold->fn(hold->arg);
    engine_lease_unref(hold->lease);
    pthread_mutex_lock(&hold->mu);
    hold->in_fn = 0;
    hold->claimed = 1;
    hold->fire_frees = hold->dropped;
    pthread_cond_signal(&hold->cv);
    int take = hold->fire_frees;
    pthread_mutex_unlock(&hold->mu);
    hold_fire_depth--;
    if (take) free(hold);
}

static void hold_drop(lease_hold *hold) {
    if (!hold) return;
    if (hold_fire_depth) {
        pthread_mutex_lock(&hold->mu);
        hold->dropped = 1;
        pthread_mutex_unlock(&hold->mu);
        return;
    }
    pthread_mutex_lock(&hold->mu);
    hold->dropped = 1;
    while (hold->in_fn) pthread_cond_wait(&hold->cv, &hold->mu);
    int fired = hold->claimed;
    int fire_frees = hold->fire_frees;
    pthread_mutex_unlock(&hold->mu);
    if (!fired) engine_lease_unref(hold->lease);
    if (!fire_frees) free(hold);
}

static lease_hold *hold_new(engine_lease *lease, void (*fn)(void *), void *arg) {
    lease_hold *hold = calloc(1, sizeof *hold);
    if (!hold) return NULL;
    engine_lease_ref(lease);
    hold->lease = lease;
    hold->fn = fn;
    hold->arg = arg;
    pthread_mutex_init(&hold->mu, NULL);
    pthread_cond_init(&hold->cv, NULL);
    return hold;
}

struct ready_arg {
    engine_manager *manager;
    engine_lease *lease;
};

static void discard_lease_arg(void *arg) {
    struct ready_arg *job = arg;
    complete_lease(job->manager, job->lease, ENGINE_WAIT_CANCELLED, NULL);
    engine_lease_unref(job->lease);
    free(arg);
}

static int submit_lease_job(
    engine_manager *manager, int readiness, void (*fn)(void *), void *arg, engine_lease *lease
) {
    engine_lease_ref(lease);
    engine_submit_fn submit = readiness ? manager->submit_readiness : manager->submit_command;
    void *ctx = readiness ? manager->submit_readiness_ctx : manager->submit_command_ctx;
    int rc = submit == executor_submit ? executor_submit_owned(fn, discard_lease_arg, arg, ctx) : submit(fn, arg, ctx);
    if (rc != 0) engine_lease_unref(lease);
    return rc;
}

static void readiness_job(void *arg) {
    struct ready_arg *ready = arg;
    engine_manager *manager = ready->manager;
    engine_lease *lease = ready->lease;
    char engine[8];
    snprintf(engine, sizeof engine, "%s", lease->engine);
    free(ready);

    pthread_mutex_lock(&manager->lock);
    int released = lease->released;
    engine_readiness_fn readiness = manager->readiness;
    void *ctx = manager->readiness_ctx;
    double timeout = manager->readiness_timeout;
    pthread_mutex_unlock(&manager->lock);
    if (!released) {
        char detail[256];
        detail[0] = '\0';
        int rc = readiness ? readiness(engine, timeout, ctx, detail, sizeof detail) : -1;
        if (rc == 1) complete_lease(manager, lease, ENGINE_WAIT_OK, NULL);
        else {
            char message[512];
            if (rc == 0) snprintf(message, sizeof message, "%s: readiness timeout", engine);
            else snprintf(message, sizeof message, "%s: %s", engine, detail[0] ? detail : "readiness failed");
            complete_lease(manager, lease, ENGINE_WAIT_FAILED, message);
        }
    }
    engine_lease_unref(lease);
}

struct start_arg {
    engine_manager *manager;
    engine_lease *lease;
};

static void start_job(void *arg) {
    struct start_arg *start = arg;
    engine_manager *manager = start->manager;
    engine_lease *lease = start->lease;
    char engine[8];
    snprintf(engine, sizeof engine, "%s", lease->engine);
    free(start);

    pthread_mutex_lock(&manager->lock);
    if (lease->released) {
        pthread_mutex_unlock(&manager->lock);
        engine_lease_unref(lease);
        return;
    }
    engine_slot *slot = find_slot(manager, engine);
    if (!slot) {
        pthread_mutex_unlock(&manager->lock);
        complete_lease(manager, lease, ENGINE_WAIT_FAILED, "missing engine");
        engine_lease_unref(lease);
        return;
    }
    int do_start = !slot->running || slot->has_error;
    if (do_start) {
        snprintf(slot->command, sizeof slot->command, "start");
        clear_error(slot);
    }
    engine_runner_fn runner = manager->runner;
    void *runner_ctx = manager->runner_ctx;
    double timeout = manager->command_timeout;
    pthread_mutex_unlock(&manager->lock);

    if (do_start) {
        char detail[256];
        detail[0] = '\0';
        int failed = !runner || runner(engine, "start", timeout, runner_ctx, detail, sizeof detail) != 0;
        if (failed) {
            char message[512];
            snprintf(message, sizeof message, "%s: %s", engine, detail[0] ? detail : "command failed");
            pthread_mutex_lock(&manager->lock);
            slot = find_slot(manager, engine);
            if (slot) {
                slot->command[0] = '\0';
                slot->running = 1;
                set_error(slot, message);
            }
            pthread_mutex_unlock(&manager->lock);
            complete_lease(manager, lease, ENGINE_WAIT_FAILED, message);
            engine_manager_sweep(manager);
            engine_lease_unref(lease);
            return;
        }
        pthread_mutex_lock(&manager->lock);
        slot = find_slot(manager, engine);
        if (slot) {
            slot->running = 1;
            slot->command[0] = '\0';
        }
        pthread_mutex_unlock(&manager->lock);
    }

    pthread_mutex_lock(&manager->lock);
    if (lease->released) {
        pthread_mutex_unlock(&manager->lock);
        engine_manager_sweep(manager);
        engine_lease_unref(lease);
        return;
    }
    struct ready_arg *ready = calloc(1, sizeof *ready);
    if (!ready) {
        pthread_mutex_unlock(&manager->lock);
        char message[64];
        snprintf(message, sizeof message, "%s: out of memory", engine);
        complete_lease(manager, lease, ENGINE_WAIT_FAILED, message);
        engine_lease_unref(lease);
        return;
    }
    ready->manager = manager;
    ready->lease = lease;
    int submitted = submit_lease_job(manager, 1, readiness_job, ready, lease);
    pthread_mutex_unlock(&manager->lock);
    if (submitted != 0) {
        free(ready);
        char message[64];
        snprintf(message, sizeof message, "%s: submit failed", engine);
        complete_lease(manager, lease, ENGINE_WAIT_FAILED, message);
    }
    engine_lease_unref(lease);
}

struct adopt_arg {
    engine_manager *manager;
    engine_activity_fn query;
    void *ctx;
};

static void schedule_idle(engine_manager *manager, engine_slot *slot) {
    if (slot->idle_timer) {
        slot->idle_timer->cancel(slot->idle_timer);
        slot->idle_timer = NULL;
    }
    char detail[256];
    detail[0] = '\0';
    engine_timer *timer = NULL;
    int rc = manager->call_later(
        manager->idle_timeout, sweep_thunk, manager, manager->call_later_ctx,
        &timer, detail, sizeof detail);
    if (rc != 0) {
        char message[512];
        snprintf(message, sizeof message, "%s: idle scheduler: %s", slot->name, detail[0] ? detail : "timer failed");
        set_error(slot, message);
        return;
    }
    slot->idle_timer = timer;
}

static void adopt_job(void *arg) {
    struct adopt_arg *adopt = arg;
    engine_manager *manager = adopt->manager;
    engine_activity_fn query = adopt->query;
    void *ctx = adopt->ctx;
    char names[ENGINE_SLOTS][8];
    int count = 0;
    pthread_mutex_lock(&manager->lock);
    count = manager->nengines;
    for (int i = 0; i < count; i++) snprintf(names[i], sizeof names[i], "%s", manager->slots[i].name);
    double timeout = manager->command_timeout;
    pthread_mutex_unlock(&manager->lock);
    free(adopt);

    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&manager->lock);
        int closed = manager->closed;
        pthread_mutex_unlock(&manager->lock);
        if (closed) return;

        int active = 0;
        char detail[256];
        detail[0] = '\0';
        int failed = 1;
        if (query) {
            int query_active = -1;
            int rc = query(names[i], timeout, ctx, &query_active, detail, sizeof detail);
            if (rc == 0 && (query_active == 0 || query_active == 1)) {
                active = query_active;
                failed = 0;
            } else if (rc == 0) {
                snprintf(detail, sizeof detail, "activity query returned no definitive state");
            }
        } else if (!detail[0]) {
            snprintf(detail, sizeof detail, "activity query missing");
        }

        pthread_mutex_lock(&manager->lock);
        if (manager->closed) {
            pthread_mutex_unlock(&manager->lock);
            return;
        }
        engine_slot *slot = find_slot(manager, names[i]);
        if (!slot) {
            pthread_mutex_unlock(&manager->lock);
            continue;
        }
        if (failed) active = 1;
        slot->running = slot->running || active;
        if (failed) {
            char message[512];
            snprintf(message, sizeof message, "%s: activity query: %s", names[i], detail[0] ? detail : "query failed");
            set_error(slot, message);
        }
        if (slot->running && slot->users == 0) {
            slot->last_release = clock_of(manager) - (slot->retire_when_idle ? manager->idle_timeout : 0);
            schedule_idle(manager, slot);
        }
        sweep_locked(manager);
        pthread_mutex_unlock(&manager->lock);
    }
}

static engine_lease *new_lease(engine_manager *manager, const char *engine) {
    engine_lease *lease = calloc(1, sizeof *lease);
    if (!lease) return NULL;
    lease->manager = manager;
    snprintf(lease->engine, sizeof lease->engine, "%s", engine);
    pthread_mutex_init(&lease->mu, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&lease->cv, &attr);
    pthread_condattr_destroy(&attr);
    lease->refs = 1;
    lease->next = manager->leases;
    manager->leases = lease;
    lease->all_next = manager->all_leases;
    manager->all_leases = lease;
    return lease;
}

static void retire_now(engine_manager *manager, const char *engine) {
    pthread_mutex_lock(&manager->lock);
    engine_slot *slot = find_slot(manager, engine);
    if (!slot || !slot->retire_when_idle || slot->users || !slot->running) {
        pthread_mutex_unlock(&manager->lock);
        return;
    }
    slot->last_release = clock_of(manager) - manager->idle_timeout;
    pthread_mutex_unlock(&manager->lock);
    engine_manager_sweep(manager);
}

static void warm_release(engine_lease *lease, void *ctx) {
    (void)ctx;
    engine_lease_release(lease);
}

static void warm_expire(void *arg) {
    engine_lease *lease = arg;
    char message[64];
    snprintf(message, sizeof message, "%s: warm timeout", lease->engine);
    complete_lease(lease->manager, lease, ENGINE_WAIT_TIMED_OUT, message);
    engine_lease_release(lease);
}

static int known_engines(const char *const *engines, size_t count) {
    if (!engines || count == 0 || count > ENGINE_SLOTS) return 0;
    for (size_t i = 0; i < count; i++) {
        if (!valid_engine_name(engines[i])) return 0;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(engines[i], engines[j]) == 0) return 0;
        }
    }
    return 1;
}

engine_manager *engine_manager_create(const engine_config *cfg) {
    engine_config defaults;
    memset(&defaults, 0, sizeof defaults);
    if (!cfg) cfg = &defaults;
    const char *fallback[] = {"stt", "tts"};
    const char *const *names = cfg->engines ? cfg->engines : fallback;
    size_t count = cfg->engines ? cfg->engine_count : 2;
    if (!known_engines(names, count)) return NULL;

    engine_manager *manager = calloc(1, sizeof *manager);
    if (!manager) return NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&manager->lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(manager);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    if (executor_init(&manager->commands, 1) != 0) {
        pthread_mutex_destroy(&manager->lock);
        free(manager);
        return NULL;
    }
    if (executor_init(&manager->readers, READER_THREADS) != 0) {
        executor_destroy(&manager->commands);
        pthread_mutex_destroy(&manager->lock);
        free(manager);
        return NULL;
    }
    manager->runner = cfg->runner;
    manager->runner_ctx = cfg->runner_ctx;
    manager->readiness = cfg->readiness;
    manager->readiness_ctx = cfg->readiness_ctx;
    manager->clock = cfg->clock;
    manager->clock_ctx = cfg->clock_ctx;
    manager->own_commands = cfg->submit_command == NULL;
    manager->own_readers = cfg->submit_readiness == NULL;
    manager->submit_command = cfg->submit_command ? cfg->submit_command : executor_submit;
    manager->submit_command_ctx = cfg->submit_command ? cfg->submit_command_ctx : &manager->commands;
    manager->submit_readiness = cfg->submit_readiness ? cfg->submit_readiness : executor_submit;
    manager->submit_readiness_ctx = cfg->submit_readiness ? cfg->submit_readiness_ctx : &manager->readers;
    manager->call_later = cfg->call_later ? cfg->call_later : default_call_later;
    manager->call_later_ctx = cfg->call_later ? cfg->call_later_ctx : manager;
    manager->idle_timeout = cfg->idle_timeout > 0 ? cfg->idle_timeout : ENGINE_IDLE_TIMEOUT;
    manager->readiness_timeout = cfg->readiness_timeout > 0 ? cfg->readiness_timeout : ENGINE_READINESS_TIMEOUT;
    manager->command_timeout = cfg->command_timeout > 0 ? cfg->command_timeout : ENGINE_COMMAND_TIMEOUT;
    manager->nengines = (int)count;
    for (size_t i = 0; i < count; i++) {
        snprintf(manager->slots[i].name, sizeof manager->slots[i].name, "%s", names[i]);
    }
    return manager;
}

void engine_manager_close(engine_manager *manager) {
    if (!manager) return;
    pthread_mutex_lock(&manager->lock);
    if (manager->shutdown) {
        pthread_mutex_unlock(&manager->lock);
        return;
    }
    manager->shutdown = 1;
    manager->closed = 1;
    while (manager->leases) engine_lease_release(manager->leases);
    for (int i = 0; i < manager->nengines; i++) {
        engine_timer *timer = manager->slots[i].idle_timer;
        manager->slots[i].idle_timer = NULL;
        if (timer) timer->cancel(timer);
    }
    int own_commands = manager->own_commands;
    int own_readers = manager->own_readers;
    pthread_mutex_unlock(&manager->lock);
    join_timers(manager);
    if (own_commands) executor_shutdown(&manager->commands);
    if (own_readers) executor_shutdown(&manager->readers);
}

void engine_manager_free(engine_manager *manager) {
    if (!manager) return;
    engine_manager_close(manager);
    for (int i = 0; i < manager->nengines; i++) {
        engine_lease *resident = manager->slots[i].resident;
        manager->slots[i].resident = NULL;
        if (resident) engine_lease_unref(resident);
    }
    engine_lease *lease = manager->all_leases;
    manager->all_leases = NULL;
    while (lease) {
        engine_lease *next = lease->all_next;
        pthread_cond_destroy(&lease->cv);
        pthread_mutex_destroy(&lease->mu);
        free(lease);
        lease = next;
    }
    executor_destroy(&manager->commands);
    executor_destroy(&manager->readers);
    pthread_mutex_destroy(&manager->lock);
    free(manager);
}

int engine_manager_reconcile(engine_manager *manager, engine_activity_fn query, void *ctx) {
    if (!manager) return ENGINE_ERR;
    pthread_mutex_lock(&manager->lock);
    if (manager->closed || manager->reconciled) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_OK;
    }
    if (!query) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_ERR;
    }
    struct adopt_arg *adopt = calloc(1, sizeof *adopt);
    if (!adopt) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_ERR;
    }
    adopt->manager = manager;
    adopt->query = query;
    adopt->ctx = ctx;
    if (manager->submit_command(adopt_job, adopt, manager->submit_command_ctx) != 0) {
        free(adopt);
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_ERR;
    }
    manager->reconciled = 1;
    pthread_mutex_unlock(&manager->lock);
    return ENGINE_OK;
}

int engine_manager_acquire(engine_manager *manager, const char *engine, engine_lease **out) {
    if (out) *out = NULL;
    if (!manager || !out) return ENGINE_ERR;
    pthread_mutex_lock(&manager->lock);
    if (manager->closed) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_CLOSED;
    }
    engine_slot *slot = find_slot(manager, engine);
    if (!slot) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_INVALID;
    }
    engine_lease *lease = new_lease(manager, engine);
    if (!lease) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_ERR;
    }
    slot->users += 1;
    slot->generation += 1;
    if (slot->idle_timer) {
        slot->idle_timer->cancel(slot->idle_timer);
        slot->idle_timer = NULL;
    }
    struct start_arg *start = calloc(1, sizeof *start);
    if (!start) {
        pthread_mutex_unlock(&manager->lock);
        complete_lease(manager, lease, ENGINE_WAIT_FAILED, "out of memory");
        engine_lease_release(lease);
        *out = lease;
        return ENGINE_OK;
    }
    start->manager = manager;
    start->lease = lease;
    if (submit_lease_job(manager, 0, start_job, start, lease) != 0) {
        free(start);
        char message[64];
        snprintf(message, sizeof message, "%s: submit failed", engine);
        pthread_mutex_unlock(&manager->lock);
        complete_lease(manager, lease, ENGINE_WAIT_FAILED, message);
        engine_lease_release(lease);
        *out = lease;
        return ENGINE_OK;
    }
    pthread_mutex_unlock(&manager->lock);
    *out = lease;
    return ENGINE_OK;
}

int engine_manager_ensure_resident(engine_manager *manager, const char *engine, engine_lease **out) {
    if (out) *out = NULL;
    if (!manager || !out) return ENGINE_ERR;
    pthread_mutex_lock(&manager->lock);
    engine_slot *slot = find_slot(manager, engine);
    if (!slot) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_INVALID;
    }
    slot->retire_when_idle = 0;
    engine_lease *current = slot->resident;
    if (current && !current->released) {
        engine_lease_ref(current);
        pthread_mutex_unlock(&manager->lock);
        *out = current;
        return ENGINE_OK;
    }
    pthread_mutex_unlock(&manager->lock);

    engine_lease *lease = NULL;
    int rc = engine_manager_acquire(manager, engine, &lease);
    if (rc != ENGINE_OK) return rc;
    pthread_mutex_lock(&manager->lock);
    slot = find_slot(manager, engine);
    current = slot ? slot->resident : NULL;
    engine_lease *extra = NULL;
    if (current && !current->released) extra = lease;
    else if (slot) {
        slot->resident = lease;
        engine_lease_ref(lease);
    }
    pthread_mutex_unlock(&manager->lock);
    if (extra) {
        engine_lease_release(extra);
        engine_lease_unref(extra);
        engine_lease_ref(current);
        *out = current;
        return ENGINE_OK;
    }
    *out = lease;
    return ENGINE_OK;
}

void engine_manager_retire(engine_manager *manager, const char *engine) {
    if (!manager || !engine) return;
    pthread_mutex_lock(&manager->lock);
    engine_slot *slot = find_slot(manager, engine);
    if (!slot) {
        pthread_mutex_unlock(&manager->lock);
        return;
    }
    engine_lease *lease = slot->resident;
    slot->resident = NULL;
    slot->retire_when_idle = 1;
    if (slot->users == 0 && slot->running) {
        slot->last_release = clock_of(manager) - manager->idle_timeout;
    }
    pthread_mutex_unlock(&manager->lock);
    if (lease) {
        engine_lease_release(lease);
        engine_lease_unref(lease);
    }
    retire_now(manager, engine);
}

void engine_lease_release(engine_lease *lease) {
    if (!lease) return;
    engine_manager *manager = lease->manager;
    pthread_mutex_lock(&manager->lock);
    if (lease->released) {
        pthread_mutex_unlock(&manager->lock);
        return;
    }
    lease->released = 1;
    engine_lease **link = &manager->leases;
    while (*link) {
        if (*link == lease) {
            *link = lease->next;
            lease->next = NULL;
            break;
        }
        link = &(*link)->next;
    }
    if (lease->timer) {
        engine_timer *timer = lease->timer;
        lease_hold *hold = lease->timer_hold;
        lease->timer = NULL;
        lease->timer_hold = NULL;
        timer->cancel(timer);
        pthread_mutex_unlock(&manager->lock);
        hold_drop(hold);
        pthread_mutex_lock(&manager->lock);
    }
    cancel_ready(lease);
    engine_slot *slot = find_slot(manager, lease->engine);
    if (slot) {
        slot->users -= 1;
        double now = clock_of(manager);
        slot->last_release = now;
        if (slot->retire_when_idle && slot->users == 0) {
            slot->last_release = now - manager->idle_timeout;
        }
        if (slot->users == 0 && !manager->closed) {
            if (slot->idle_timer) {
                slot->idle_timer->cancel(slot->idle_timer);
                slot->idle_timer = NULL;
            }
            char detail[256];
            detail[0] = '\0';
            engine_timer *timer = NULL;
            int rc = manager->call_later(
                manager->idle_timeout, sweep_thunk, manager, manager->call_later_ctx,
                &timer, detail, sizeof detail);
            if (rc != 0) {
                char message[512];
                snprintf(
                    message, sizeof message, "%s: idle scheduler: %s",
                    slot->name, detail[0] ? detail : "timer failed");
                set_error(slot, message);
            } else {
                slot->idle_timer = timer;
            }
            sweep_locked(manager);
        }
    }
    pthread_mutex_unlock(&manager->lock);
}

int engine_manager_warm(
    engine_manager *manager, const char *const *engines, size_t count, double timeout,
    engine_lease **out, size_t cap, size_t *out_count
) {
    if (out_count) *out_count = 0;
    if (!manager || !out || !out_count) return ENGINE_ERR;
    const char *owned[ENGINE_SLOTS];
    const char *fallback[ENGINE_SLOTS];
    size_t n = count;
    if (!engines) {
        n = (size_t)manager->nengines;
        for (size_t i = 0; i < n; i++) fallback[i] = manager->slots[i].name;
        engines = fallback;
    }
    if (timeout <= 0 || n == 0 || n > cap || n > ENGINE_SLOTS) return ENGINE_INVALID;
    for (size_t i = 0; i < n; i++) {
        if (!find_slot(manager, engines[i])) return ENGINE_INVALID;
        owned[i] = engines[i];
    }
    engine_lease *leases[ENGINE_SLOTS] = {0};
    for (size_t i = 0; i < n; i++) {
        int rc = engine_manager_acquire(manager, owned[i], &leases[i]);
        if (rc != ENGINE_OK) {
            for (size_t j = 0; j < i; j++) {
                engine_lease_release(leases[j]);
                engine_lease_unref(leases[j]);
            }
            return rc;
        }
        lease_on_done(leases[i], warm_release, NULL);
        pthread_mutex_lock(&manager->lock);
        if (!leases[i]->released) {
            char detail[256];
            detail[0] = '\0';
            engine_timer *timer = NULL;
            lease_hold *hold = hold_new(leases[i], warm_expire, leases[i]);
            int later = hold ? manager->call_later(
                timeout, hold_fire, hold, manager->call_later_ctx,
                &timer, detail, sizeof detail) : -1;
            if (later != 0) {
                pthread_mutex_unlock(&manager->lock);
                hold_drop(hold);
                for (size_t j = 0; j <= i; j++) {
                    engine_lease_release(leases[j]);
                    engine_lease_unref(leases[j]);
                }
                return ENGINE_ERR;
            }
            leases[i]->timer = timer;
            leases[i]->timer_hold = hold;
        }
        pthread_mutex_unlock(&manager->lock);
        out[i] = leases[i];
    }
    *out_count = n;
    return ENGINE_OK;
}

int engine_manager_set_population(engine_manager *manager, int sessions, int pending_admissions) {
    if (!manager) return ENGINE_ERR;
    if (sessions < 0 || pending_admissions < 0) return ENGINE_INVALID;
    pthread_mutex_lock(&manager->lock);
    if (manager->sessions > 0 && sessions == 0) manager->early_stop = 1;
    if (sessions > 0) manager->early_stop = 0;
    manager->sessions = sessions;
    manager->admissions = pending_admissions;
    manager->population_generation += 1;
    sweep_locked(manager);
    pthread_mutex_unlock(&manager->lock);
    return ENGINE_OK;
}

void engine_manager_sweep(engine_manager *manager) {
    if (!manager) return;
    pthread_mutex_lock(&manager->lock);
    sweep_locked(manager);
    pthread_mutex_unlock(&manager->lock);
}

int engine_manager_state(engine_manager *manager, const char *engine, engine_state *out) {
    if (!manager || !out) return ENGINE_ERR;
    pthread_mutex_lock(&manager->lock);
    engine_slot *slot = find_slot(manager, engine);
    if (!slot) {
        pthread_mutex_unlock(&manager->lock);
        return ENGINE_INVALID;
    }
    copy_state(slot, out);
    pthread_mutex_unlock(&manager->lock);
    return ENGINE_OK;
}

const char *engine_lease_name(const engine_lease *lease) {
    return lease ? lease->engine : NULL;
}

int engine_lease_done(engine_lease *lease) {
    return lease && lease_is_ready(lease);
}

int engine_lease_cancelled(engine_lease *lease) {
    if (!lease) return 0;
    pthread_mutex_lock(&lease->mu);
    int cancelled = lease->ready && lease->status == ENGINE_WAIT_CANCELLED;
    pthread_mutex_unlock(&lease->mu);
    return cancelled;
}

int engine_lease_released(engine_lease *lease) {
    if (!lease) return 0;
    pthread_mutex_lock(&lease->manager->lock);
    int released = lease->released;
    pthread_mutex_unlock(&lease->manager->lock);
    return released;
}

int engine_lease_wait(engine_lease *lease, int timeout_ms, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!lease) return ENGINE_WAIT_FAILED;
    pthread_mutex_lock(&lease->mu);
    if (!lease->ready && timeout_ms != 0) {
        if (timeout_ms < 0) {
            while (!lease->ready) pthread_cond_wait(&lease->cv, &lease->mu);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            add_ms(&ts, timeout_ms);
            while (!lease->ready) {
                int rc = pthread_cond_timedwait(&lease->cv, &lease->mu, &ts);
                if (rc == ETIMEDOUT) break;
            }
        }
    }
    int status = lease->ready ? lease->status : ENGINE_WAIT_UNREADY;
    if (err && err_cap && lease->ready) snprintf(err, err_cap, "%s", lease->error);
    pthread_mutex_unlock(&lease->mu);
    return status;
}

static void strip_inplace(char *text) {
    char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t n = strlen(text);
    while (n > 0 && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
}

int engine_active_from_text(const char *engine, const char *text, int *resident, char *err, size_t err_cap) {
    if (resident) *resident = 0;
    if (!valid_engine_name(engine) || !resident) return ENGINE_INVALID;
    char copy[128];
    snprintf(copy, sizeof copy, "%s", text ? text : "");
    strip_inplace(copy);
    if (strcmp(copy, "inactive") == 0 || strcmp(copy, "failed") == 0) {
        *resident = 0;
        return ENGINE_OK;
    }
    if (strcmp(copy, "active") == 0 || strcmp(copy, "activating") == 0 ||
        strcmp(copy, "reloading") == 0 || strcmp(copy, "deactivating") == 0 ||
        strcmp(copy, "refreshing") == 0) {
        *resident = 1;
        return ENGINE_OK;
    }
    if (err && err_cap) snprintf(err, err_cap, "%s: unknown ActiveState '%s'", engine, copy);
    return ENGINE_ERR;
}

static int fill_unit(const char *engine, char *unit, size_t cap) {
    if (!valid_engine_name(engine)) return -1;
    snprintf(unit, cap, "pi-voice-%s.service", engine);
    return 0;
}

int engine_systemctl_active(
    const char *engine, double timeout, engine_subprocess_fn run, void *ctx,
    int *resident, char *err, size_t err_cap
) {
    if (resident) *resident = 0;
    if (!run || !resident) {
        if (err && err_cap) snprintf(err, err_cap, "%s: systemctl not injected", engine ? engine : "");
        return ENGINE_ERR;
    }
    char unit[64];
    if (fill_unit(engine, unit, sizeof unit) != 0) return ENGINE_INVALID;
    engine_subprocess_call call;
    memset(&call, 0, sizeof call);
    call.argv[0] = "systemctl";
    call.argv[1] = "--user";
    call.argv[2] = "show";
    call.argv[3] = "--property=ActiveState";
    call.argv[4] = "--value";
    call.argv[5] = unit;
    call.argc = 6;
    call.timeout = timeout;
    call.check = 1;
    char stdout_buf[256];
    stdout_buf[0] = '\0';
    if (run(&call, stdout_buf, sizeof stdout_buf, ctx) != 0) {
        if (err && err_cap) snprintf(err, err_cap, "%s: activity query failed", engine);
        return ENGINE_ERR;
    }
    stdout_buf[sizeof stdout_buf - 1] = '\0';
    return engine_active_from_text(engine, stdout_buf, resident, err, err_cap);
}

int engine_systemctl(
    const char *engine, const char *command, double timeout,
    engine_subprocess_fn run, void *ctx, char *err, size_t err_cap
) {
    if (!run || !command || !command[0]) {
        if (err && err_cap) snprintf(err, err_cap, "systemctl not injected");
        return ENGINE_ERR;
    }
    char unit[64];
    if (fill_unit(engine, unit, sizeof unit) != 0) return ENGINE_INVALID;
    engine_subprocess_call call;
    memset(&call, 0, sizeof call);
    call.argv[0] = "systemctl";
    call.argv[1] = "--user";
    call.argv[2] = command;
    call.argv[3] = unit;
    call.argc = 4;
    call.timeout = timeout;
    call.check = 1;
    char stdout_buf[64];
    stdout_buf[0] = '\0';
    if (run(&call, stdout_buf, sizeof stdout_buf, ctx) != 0) {
        if (err && err_cap) snprintf(err, err_cap, "%s: %s failed", engine, command);
        return ENGINE_ERR;
    }
    return ENGINE_OK;
}
