/* Exercise production-only ownership adapters, without running the daemon. */
#define main voice_cli_entry_for_tests
#include "../src/main.c"
#undef main

static int fake_engine_command(const char *engine, const char *command, double timeout,
                               void *ctx, char *err, size_t cap) {
    (void)engine; (void)command; (void)timeout; (void)ctx; (void)err; (void)cap;
    return 0;
}

static int fake_engine_ready(const char *engine, double timeout, void *ctx, char *err, size_t cap) {
    (void)engine; (void)timeout; (void)err; (void)cap;
    atomic_int *ready = ctx;
    while (!atomic_load(ready)) usleep(1000);
    return 1;
}

static int await_no_leases(engine_manager *manager) {
    for (int i = 0; i < 200; i++) {
        if (engine_manager_live_leases(manager) == 0) return 0;
        usleep(1000);
    }
    return 1;
}

int test_main_adapters(void) {
    int failed = 0;
    atomic_int ready = 1;
    engine_config config = {.runner = fake_engine_command, .readiness = fake_engine_ready,
                            .readiness_ctx = &ready};
    engine_manager *manager = engine_manager_create(&config);
    if (!manager) return 1;
    char err[256] = {0};
    for (int i = 0; i < 32; i++) {
        void *lease = NULL;
        if (engine_acquire(manager, "stt", &lease, err, sizeof err) || !lease) {
            failed++;
            break;
        }
        if (engine_wait(lease, 1000, err, sizeof err) != 0) failed++;
        engine_release(lease);
    }
    if (await_no_leases(manager)) {
        fprintf(stderr, "Production release retained completed engine leases\n");
        failed++;
    }
    engine_state state;
    engine_resident(manager, "tts");
    if (engine_manager_state(manager, "tts", &state) || state.users != 1) {
        fprintf(stderr, "Production observer release cancelled residency\n");
        failed++;
    }
    engine_retire(manager, "tts");
    if (await_no_leases(manager)) failed++;
    atomic_store(&ready, 0);
    engine_warm(manager);
    if (engine_manager_state(manager, "stt", &state) || state.users != 1) {
        fprintf(stderr, "Production warm did not hold stt until readiness\n");
        failed++;
    }
    if (engine_manager_state(manager, "tts", &state) || state.users != 1) {
        fprintf(stderr, "Production warm did not hold tts until readiness\n");
        failed++;
    }
    atomic_store(&ready, 1);
    if (await_no_leases(manager)) failed++;
    engine_manager_close(manager);
    if (engine_manager_live_leases(manager) != 0) failed++;
    engine_manager_free(manager);

    const char *only_stt[] = {"stt"};
    config.engines = only_stt;
    config.engine_count = 1;
    manager = engine_manager_create(&config);
    if (!manager) return failed + 1;
    atomic_store(&ready, 0);
    engine_warm(manager);
    if (engine_manager_state(manager, "stt", &state) || state.users != 1) {
        fprintf(stderr, "Production warm failed without a local speech engine\n");
        failed++;
    }
    atomic_store(&ready, 1);
    if (await_no_leases(manager)) failed++;
    engine_manager_close(manager);
    engine_manager_free(manager);
    return failed;
}
