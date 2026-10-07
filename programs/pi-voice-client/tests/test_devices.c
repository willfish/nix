#define _POSIX_C_SOURCE 200809L
#include "capture.h"
#include "devices.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void fail(const char *name, const char *detail) {
    fprintf(stderr, "FAIL %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
    failures++;
}

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *webcam_json =
    "["
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"webcam\",\"node.description\":\"Webcam\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":false}]}}},"
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"headset\",\"node.description\":\"Headset\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":true}]}}},"
    "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
    "{\"key\":\"default.audio.source\",\"value\":{\"name\":\"webcam\"}}]}]";

static const char *headset_default_json =
    "["
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"webcam\",\"node.description\":\"Webcam\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":false}]}}},"
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"headset\",\"node.description\":\"Headset\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":true}]}}},"
    "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
    "{\"key\":\"default.audio.source\",\"value\":{\"name\":\"headset\"}}]}]";

static const char *no_headset_json =
    "["
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"webcam\",\"node.description\":\"Webcam\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":false}]}}},"
    "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
    "{\"key\":\"default.audio.source\",\"value\":{\"name\":\"webcam\"}}]}]";

static const char *disconnected_json =
    "["
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"webcam\",\"node.description\":\"Webcam\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":false}]}}},"
    "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
    "{\"key\":\"default.audio.source\",\"value\":{\"name\":\"disconnected\"}}]}]";

static const char *string_metadata_json =
    "["
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"webcam\",\"node.description\":\"Webcam\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":false}]}}},"
    "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
    "{\"key\":\"default.audio.source\",\"value\":\"{\\\"name\\\":\\\"webcam\\\"}\"}]},"
    "{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"speakers\",\"node.description\":\"Desktop audio\",\"media.class\":\"Audio/Sink\"},"
    "\"params\":{\"Props\":[{\"mute\":false}]}}}]";

static const char *usb_json =
    "[{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{"
    "\"node.name\":\"usb.microphone\",\"node.description\":\"Test microphone\","
    "\"media.class\":\"Audio/Source\"},\"params\":{\"Props\":[{\"mute\":false}]}}}]";

typedef struct Probe {
    const char *json;
    int fail;
    int calls;
    double timeout;
    int argc;
    char *argv0;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int began;
    int release;
    int slow;
} Probe;

static int probe_run(char *const *argv, double timeout, MicRunResult *out, void *user) {
    Probe *probe = user;
    pthread_mutex_lock(&probe->mu);
    probe->calls++;
    probe->timeout = timeout;
    probe->argc = 0;
    while (argv[probe->argc]) probe->argc++;
    free(probe->argv0);
    probe->argv0 = argv[0] ? strdup(argv[0]) : NULL;
    const char *json = probe->json;
    int fail_run = probe->fail;
    pthread_mutex_unlock(&probe->mu);
    memset(out, 0, sizeof *out);
    if (fail_run) return -1;
    out->ok = 1;
    out->exit_code = 0;
    out->stdout_text = strdup(json ? json : "");
    return out->stdout_text ? 0 : -1;
}

static int slow_run(char *const *argv, double timeout, MicRunResult *out, void *user) {
    Probe *probe = user;
    pthread_mutex_lock(&probe->mu);
    probe->began = 1;
    pthread_cond_broadcast(&probe->cv);
    while (!probe->release) pthread_cond_wait(&probe->cv, &probe->mu);
    pthread_mutex_unlock(&probe->mu);
    return probe_run(argv, timeout, out, user);
}

static void probe_init(Probe *probe) {
    memset(probe, 0, sizeof *probe);
    pthread_mutex_init(&probe->mu, NULL);
    pthread_cond_init(&probe->cv, NULL);
    probe->json = webcam_json;
}

static void probe_destroy(Probe *probe) {
    pthread_mutex_lock(&probe->mu);
    probe->release = 1;
    pthread_cond_broadcast(&probe->cv);
    pthread_mutex_unlock(&probe->mu);
    free(probe->argv0);
    pthread_cond_destroy(&probe->cv);
    pthread_mutex_destroy(&probe->mu);
}

static int same_status(const MicStatus *a, const MicStatus *b) {
    if (a->muted != b->muted || a->muted_known != b->muted_known || a->missing != b->missing) return 0;
    if (strcmp(a->name ? a->name : "", b->name ? b->name : "") != 0) return 0;
    if (strcmp(a->target ? a->target : "", b->target ? b->target : "") != 0) return 0;
    if (strcmp(a->preferred ? a->preferred : "", b->preferred ? b->preferred : "") != 0) return 0;
    if (strcmp(a->error ? a->error : "", b->error ? b->error : "") != 0) return 0;
    return 1;
}

static void test_default_routing(void) {
    Probe probe;
    probe_init(&probe);
    MicrophoneMonitor *monitor = mic_monitor_new(NULL, probe_run, &probe);
    MicStatus status;
    if (mic_resolve(monitor, &status) != 0) fail("default", "resolve");
    else {
        if (!status.name || strcmp(status.name, "Webcam") != 0) fail("default", "name");
        if (status.target) fail("default", "target");
        if (status.missing) fail("default", "missing");
        if (!status.muted_known || status.muted) fail("default", "muted");
        if (probe.argc != 1 || !probe.argv0 || strcmp(probe.argv0, "pw-dump") != 0) fail("default", "argv");
        if (probe.timeout > 2) fail("default", "timeout");
    }
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_preferred_mute(void) {
    Probe probe;
    probe_init(&probe);
    MicrophoneMonitor *monitor = mic_monitor_new("headset", probe_run, &probe);
    MicStatus status;
    mic_resolve(monitor, &status);
    if (!status.name || strcmp(status.name, "Headset") != 0) fail("preferred", "name");
    if (!status.target || strcmp(status.target, "headset") != 0) fail("preferred", "target");
    if (!status.muted_known || !status.muted) fail("preferred", "muted");
    if (status.missing) fail("preferred", "missing");
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_unplugged_preference(void) {
    Probe probe;
    probe_init(&probe);
    MicrophoneMonitor *monitor = mic_monitor_new("headset", probe_run, &probe);
    MicStatus status;
    mic_resolve(monitor, &status);
    if (!status.target || strcmp(status.target, "headset") != 0) fail("unplugged", "first target");
    mic_status_free(&status);
    probe.json = no_headset_json;
    mic_resolve(monitor, &status);
    if (status.target) fail("unplugged", "stale target");
    if (!status.name || strcmp(status.name, "Webcam") != 0) fail("unplugged", "name");
    if (!status.missing) fail("unplugged", "missing");
    if (status.error) fail("unplugged", status.error);
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_probe_failure_drops_stale_target(void) {
    Probe probe;
    probe_init(&probe);
    MicrophoneMonitor *monitor = mic_monitor_new("headset", probe_run, &probe);
    MicStatus status;
    mic_resolve(monitor, &status);
    if (!status.target || strcmp(status.target, "headset") != 0) fail("stale", "first target");
    mic_status_free(&status);
    probe.fail = 1;
    mic_resolve(monitor, &status);
    if (status.target) fail("stale", "target reused");
    if (!status.missing) fail("stale", "missing");
    if (!status.error || !strstr(status.error, "unavailable")) fail("stale", status.error ? status.error : "null");
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_status_does_not_wait(void) {
    Probe probe;
    probe_init(&probe);
    probe.slow = 1;
    MicrophoneMonitor *monitor = mic_monitor_new(NULL, slow_run, &probe);
    double started = mono();
    MicStatus first;
    if (mic_status(monitor, &first) != 0) fail("status", "status");
    if (mono() - started >= 0.1) fail("status", "blocked");
    pthread_mutex_lock(&probe.mu);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 500000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    while (!probe.began) {
        if (pthread_cond_timedwait(&probe.cv, &probe.mu, &ts) != 0) break;
    }
    int began = probe.began;
    pthread_mutex_unlock(&probe.mu);
    if (!began) fail("status", "probe did not start");
    for (int i = 0; i < 20; i++) {
        MicStatus again;
        mic_status(monitor, &again);
        if (!same_status(&first, &again)) fail("status", "changed");
        mic_status_free(&again);
    }
    pthread_mutex_lock(&probe.mu);
    probe.release = 1;
    pthread_cond_broadcast(&probe.cv);
    pthread_mutex_unlock(&probe.mu);
    double deadline = mono() + 1;
    int saw = 0;
    while (mono() < deadline) {
        MicStatus now;
        mic_status(monitor, &now);
        saw = now.name && strcmp(now.name, "Webcam") == 0;
        mic_status_free(&now);
        if (saw) break;
        struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }
    if (!saw) fail("status", "did not refresh");
    if (probe.calls != 1) fail("status", "duplicate workers");
    mic_status_free(&first);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_resolve_updates_cached_status(void) {
    Probe probe;
    probe_init(&probe);
    MicrophoneMonitor *monitor = mic_monitor_new(NULL, probe_run, &probe);
    MicStatus status;
    mic_resolve(monitor, &status);
    if (!status.name || strcmp(status.name, "Webcam") != 0) fail("refresh", "first");
    mic_status_free(&status);
    probe.json = headset_default_json;
    mic_status(monitor, &status);
    if (!status.name || strcmp(status.name, "Webcam") != 0) fail("refresh", "cached");
    mic_status_free(&status);
    mic_resolve(monitor, &status);
    if (!status.name || strcmp(status.name, "Headset") != 0) fail("refresh", "resolved");
    mic_status_free(&status);
    mic_status(monitor, &status);
    if (!status.name || strcmp(status.name, "Headset") != 0) fail("refresh", "status");
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_missing_default(void) {
    Probe probe;
    probe_init(&probe);
    probe.json = disconnected_json;
    MicrophoneMonitor *monitor = mic_monitor_new(NULL, probe_run, &probe);
    MicStatus status;
    mic_resolve(monitor, &status);
    if (status.target) fail("missing default", "target");
    if (!status.name || strcmp(status.name, "System default") != 0) fail("missing default", "name");
    if (!status.error || strcmp(status.error, "No default microphone available") != 0) {
        fail("missing default", status.error ? status.error : "null");
    }
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_string_metadata_excludes_sinks(void) {
    Probe probe;
    probe_init(&probe);
    probe.json = string_metadata_json;
    MicrophoneMonitor *monitor = mic_monitor_new("speakers", probe_run, &probe);
    MicStatus status;
    mic_resolve(monitor, &status);
    if (!status.name || strcmp(status.name, "Webcam") != 0) fail("sink", "name");
    if (!status.missing) fail("sink", "missing");
    if (status.target) fail("sink", "target");
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static char **recorded_argv;
static int recorded_argc;

static int record_target_spawn(char *const *argv, int stderr_fd, CaptureProc *out, void *user) {
    (void)user;
    for (int i = 0; i < recorded_argc; i++) free(recorded_argv[i]);
    free(recorded_argv);
    recorded_argc = 0;
    while (argv[recorded_argc]) recorded_argc++;
    recorded_argv = calloc((size_t)recorded_argc, sizeof *recorded_argv);
    for (int i = 0; i < recorded_argc; i++) recorded_argv[i] = strdup(argv[i]);
    char *producer[] = {"node", "-e", "require('node:fs').writeSync(1,Buffer.alloc(32))", NULL};
    return capture_spawn_default(producer, stderr_fd, out);
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

static void test_repeated_probes_keep_latest_snapshot(void) {
    Probe probe;
    MicrophoneMonitor *monitor;
    long before;
    long after;
    probe_init(&probe);
    monitor = mic_monitor_new("headset", probe_run, &probe);
    before = vm_size_kb();
    for (int i = 0; i < 8; i++) {
        MicStatus status;
        int want_headset = (i % 2) == 0;
        probe.json = want_headset ? webcam_json : no_headset_json;
        if (mic_resolve(monitor, &status) != 0) fail("repeated probe", "resolve");
        else if (want_headset) {
            if (!status.target || strcmp(status.target, "headset") != 0 || !status.muted_known || !status.muted)
                fail("repeated probe", "headset");
        } else if (status.target || !status.name || strcmp(status.name, "Webcam") != 0 || !status.missing) {
            fail("repeated probe", "fallback");
        }
        mic_status_free(&status);
    }
    after = vm_size_kb();
    /* Threads ignores exited-but-unjoined threads. Each one still retains a
       stack mapping, so eight unreaped probes grow VmSize by tens of MB. */
    if (before < 0 || after < 0 || after > before + 32768) fail("repeated probe", "probe stacks retained");
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_duplicate_source_name_uses_the_later_node(void) {
    static const char *json =
        "[{"
        "\"info\":{\"props\":{\"node.name\":\"webcam\",\"node.description\":\"First\","
        "\"media.class\":\"Audio/Source\"},\"params\":{\"Props\":[{\"mute\":false}]}}},"
        "{\"info\":{\"props\":{\"node.name\":\"webcam\",\"node.description\":\"Second\","
        "\"media.class\":\"Audio/Source\"},\"params\":{\"Props\":[{\"mute\":true}]}}},"
        "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
        "{\"key\":\"default.audio.source\",\"value\":{\"name\":\"webcam\"}}]}]";
    Probe probe;
    MicrophoneMonitor *monitor;
    MicStatus status;
    probe_init(&probe);
    probe.json = json;
    monitor = mic_monitor_new(NULL, probe_run, &probe);
    if (mic_resolve(monitor, &status) != 0) fail("duplicate source", "resolve");
    else if (!status.name || strcmp(status.name, "Second") != 0 || !status.muted_known || !status.muted) {
        fail("duplicate source", status.name ? status.name : "null");
    }
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_non_object_default_metadata_is_unavailable(void) {
    static const char *json =
        "[{\"info\":{\"props\":{\"node.name\":\"webcam\",\"node.description\":\"Webcam\","
        "\"media.class\":\"Audio/Source\"},\"params\":{\"Props\":[{\"mute\":false}]}}},"
        "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
        "{\"key\":\"default.audio.source\",\"value\":\"null\"}]}]"
    ;
    Probe probe;
    MicrophoneMonitor *monitor;
    MicStatus status;
    probe_init(&probe);
    probe.json = json;
    monitor = mic_monitor_new(NULL, probe_run, &probe);
    if (mic_resolve(monitor, &status) != 0) fail("null metadata", "resolve");
    else if (!status.error || strcmp(status.error, "Microphone details unavailable") != 0 || status.target) {
        fail("null metadata", status.error ? status.error : "null");
    }
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_object_props_fail_the_probe(void) {
    static const char *json =
        "[{\"info\":{\"props\":{\"node.name\":\"webcam\",\"node.description\":\"Webcam\","
        "\"media.class\":\"Audio/Source\"},\"params\":{\"Props\":{\"mute\":true}}},"
        "\"props\":{\"metadata.name\":\"default\"}},"
        "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":["
        "{\"key\":\"default.audio.source\",\"value\":{\"name\":\"webcam\"}}]}]"
    ;
    Probe probe;
    MicrophoneMonitor *monitor;
    MicStatus status;
    probe_init(&probe);
    probe.json = json;
    monitor = mic_monitor_new(NULL, probe_run, &probe);
    if (mic_resolve(monitor, &status) != 0) fail("props object", "resolve");
    else if (!status.error || strcmp(status.error, "Microphone details unavailable") != 0) {
        fail("props object", status.error ? status.error : "null");
    }
    mic_status_free(&status);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
}

static void test_capture_resolves_preferred_device_and_reports_current_name(void) {
    Probe probe;
    probe_init(&probe);
    probe.json = usb_json;
    MicrophoneMonitor *monitor = mic_monitor_new("usb.microphone", probe_run, &probe);
    MicStatus resolved;
    if (mic_resolve(monitor, &resolved) != 0) fail("preferred capture", "resolve");
    char dir[] = "/tmp/voice-dev-XXXXXX";
    if (!mkdtemp(dir)) {
        fail("preferred capture", "mkdtemp");
        mic_status_free(&resolved);
        mic_monitor_free(monitor);
        probe_destroy(&probe);
        return;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/capture.wav", dir);
    capture_test_reset();
    capture_spawn_hook = record_target_spawn;
    PipeWireCapture *capture = capture_open(path, NULL, resolved.target);
    int target_at = -1;
    for (int i = 0; i < recorded_argc; i++) {
        if (strcmp(recorded_argv[i], "--target") == 0) target_at = i;
    }
    if (!capture || target_at < 0 || target_at + 1 >= recorded_argc
        || strcmp(recorded_argv[target_at + 1], "usb.microphone") != 0) {
        fail("preferred capture", "target");
    }
    MicStatus cached;
    mic_status(monitor, &cached);
    if (!cached.name || strcmp(cached.name, "Test microphone") != 0) fail("preferred capture", "name");
    mic_status_free(&cached);
    capture_free(capture);
    unlink(path);
    rmdir(dir);
    for (int i = 0; i < recorded_argc; i++) free(recorded_argv[i]);
    free(recorded_argv);
    recorded_argv = NULL;
    recorded_argc = 0;
    mic_status_free(&resolved);
    mic_monitor_free(monitor);
    probe_destroy(&probe);
    capture_test_reset();
}

int test_devices(void) {
    failures = 0;
    test_default_routing();
    test_preferred_mute();
    test_unplugged_preference();
    test_probe_failure_drops_stale_target();
    test_status_does_not_wait();
    test_resolve_updates_cached_status();
    test_missing_default();
    test_string_metadata_excludes_sinks();
    test_repeated_probes_keep_latest_snapshot();
    test_duplicate_source_name_uses_the_later_node();
    test_non_object_default_metadata_is_unavailable();
    test_object_props_fail_the_probe();
    test_capture_resolves_preferred_device_and_reports_current_name();
    return failures;
}
