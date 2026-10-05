#define _POSIX_C_SOURCE 200809L
#include "runtime_adapters.h"
#include "attachments.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <yyjson.h>

static int failures;
static const char *test_name;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, test_name, #cond); failures++; } } while (0)

static char requests[8][1024];
static atomic_int nrequests;

static int read_line(int conn, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        struct pollfd pfd = {.fd = conn, .events = POLLIN};
        if (poll(&pfd, 1, 1000) <= 0) return -1;
        ssize_t got = read(conn, buf + n, 1);
        if (got <= 0) return -1;
        if (buf[n] == '\n') {
            buf[n] = 0;
            return 0;
        }
        n++;
    }
    return -1;
}

static void *serve_lines(void *arg) {
    int server = *(int *)arg;
    for (int i = 0; i < 6; i++) {
        struct pollfd pfd = {.fd = server, .events = POLLIN};
        if (poll(&pfd, 1, 500) <= 0) break;
        int conn = accept(server, NULL, NULL);
        if (conn < 0) break;
        char buf[1024];
        if (read_line(conn, buf, sizeof buf) != 0) {
            close(conn);
            continue;
        }
        int slot = atomic_load(&nrequests);
        if (slot < 8) {
            snprintf(requests[slot], sizeof requests[0], "%s", buf);
            atomic_store(&nrequests, slot + 1);
        }
        if (strstr(buf, "drop")) {
            close(conn);
            continue;
        }
        const char *reply = "{\"ok\":true,\"result\":{}}\n";
        if (strstr(buf, "\"command\":\"status\""))
            reply = "{\"ok\":true,\"result\":{\"session\":\"s\",\"pid\":1,\"ready\":true,\"state\":\"idle\",\"accepts_input\":true}}\n";
        if (strstr(buf, "pane.send_input")) {
            const char *id = strstr(buf, "\"id\":\"");
            char token[40] = "x";
            if (id) {
                snprintf(token, sizeof token, "%.32s", id + 6);
                char *end = strchr(token, '"');
                if (end) *end = 0;
            }
            char line[96];
            snprintf(line, sizeof line, "{\"id\":\"%s\"}\n", token);
            if (write(conn, line, strlen(line)) < 0) {
                close(conn);
                continue;
            }
            close(conn);
            continue;
        }
        if (write(conn, reply, strlen(reply)) < 0) {
            close(conn);
            continue;
        }
        close(conn);
    }
    return NULL;
}

static int listen_sock(char *path, size_t cap, int *server) {
    char dir[] = "/tmp/vadpXXXXXX";
    if (!mkdtemp(dir)) return -1;
    if ((size_t)snprintf(path, cap, "%s/s.sock", dir) >= cap) return -1;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    *server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (bind(*server, (struct sockaddr *)&addr, sizeof addr) != 0) return -1;
    if (listen(*server, 4) != 0) return -1;
    chmod(path, 0600);
    return 0;
}

static void test_pi_lost_ack_is_uncertain_and_not_retried(void) {
    test_name = "pi_uncertain";
    char path[128];
    int server = -1;
    CHECK(listen_sock(path, sizeof path, &server) == 0);
    nrequests = 0;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, serve_lines, &server) == 0);
    voice_target_fields fields = {.adapter_socket = path, .token = "secret", .session = "s", .pid = 1};
    voice_target *target = voice_target_new(&fields);
    voice_pi *pi = voice_pi_new();
    char err[256] = {0};
    yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(mut);
    yyjson_mut_doc_set_root(mut, obj);
    yyjson_mut_obj_add_str(mut, obj, "drop", "1");
    yyjson_doc *extra = yyjson_mut_doc_imut_copy(mut, NULL);
    yyjson_mut_doc_free(mut);
    int rc = voice_pi_request(pi, target, "stage", extra, NULL, err, sizeof err);
    CHECK(rc == VOICE_UNCERTAIN);
    CHECK(strstr(err, "confirm"));
    int seen = nrequests;
    rc = voice_pi_request(pi, target, "stage", extra, NULL, err, sizeof err);
    CHECK(rc == VOICE_UNCERTAIN);
    CHECK(nrequests == seen + 1);
    yyjson_doc *status = NULL;
    fields.session = "other";
    voice_target *wrong = voice_target_new(&fields);
    rc = voice_pi_validate_target(pi, wrong, &status, err, sizeof err);
    CHECK(rc == VOICE_REJECTED);
    CHECK(strstr(err, "rebind"));
    CHECK(status == NULL);
    yyjson_doc_free(extra);
    voice_target_free(wrong);
    voice_target_free(target);
    voice_pi_free(pi);
    pthread_join(thread, NULL);
    close(server);
    unlink(path);
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = 0;
        rmdir(path);
    }
}

static int fake_herdr(const ipc_process_request *request, ipc_process_result *result, void *user, char *err, size_t err_cap) {
    (void)user;
    (void)err;
    (void)err_cap;
    memset(result, 0, sizeof *result);
    result->exit_code = 0;
    const char *kind = "other";
    for (int i = 0; i < request->argc; i++) {
        if (strcmp(request->argv[i], "process-info") == 0) kind = "info";
        if (strcmp(request->argv[i], "prompt") == 0) kind = "prompt";
        if (strcmp(request->argv[i], "get") == 0) kind = "get";
        CHECK(strcmp(request->argv[i], "send-text") != 0);
    }
    const char *body = "{\"result\":{\"process_info\":{\"foreground_processes\":[{\"pid\":1,\"name\":\"not-pi\"}]}}}";
    if (strcmp(kind, "get") == 0) body = "{\"result\":{\"agent\":{\"agent\":\"pi\",\"agent_status\":\"working\"}}}";
    if (strcmp(kind, "prompt") == 0) body = "{\"result\":{\"sent\":true}}";
    result->stdout_bytes = (unsigned char *)strdup(body);
    result->stdout_len = strlen(body);
    CHECK(request->env && request->env_count == 1);
    CHECK(strcmp(request->env[0].key, "HERDR_SOCKET_PATH") == 0);
    return IPC_OK;
}

static void test_herdr_input_has_empty_keys(void) {
    test_name = "herdr_no_keys";
    char path[128];
    int server = -1;
    CHECK(listen_sock(path, sizeof path, &server) == 0);
    nrequests = 0;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, serve_lines, &server) == 0);
    voice_target_fields fields = {.socket = path, .pane = "w1:p1", .pid = (int)getpid(), .harness = "pi", .start = "ignored"};
    char start[64];
    if (process_start_ticks(fields.pid, start, sizeof start) == 0) fields.start = start;
    voice_target *target = voice_target_new(&fields);
    voice_herdr *herdr = voice_herdr_new("herdr");
    char err[256] = {0};
    int rc = voice_herdr_input(herdr, target, "Retain this speech", err, sizeof err);
    CHECK(rc == VOICE_OK);
    CHECK(nrequests == 1);
    CHECK(strstr(requests[0], "pane.send_input"));
    CHECK(strstr(requests[0], "\"keys\":[]"));
    CHECK(strstr(requests[0], "send_text") == NULL);
    pthread_join(thread, NULL);
    close(server);
    unlink(path);
    voice_herdr_set_process(herdr, fake_herdr, NULL);
    yyjson_doc *agent = NULL;
    rc = voice_herdr_validate_target(herdr, target, &agent, err, sizeof err);
    CHECK(rc == VOICE_REJECTED);
    CHECK(strstr(err, "foreground"));
    yyjson_doc_free(agent);
    const char *args[] = {"agent", "prompt", "w1:p1", " "};
    rc = voice_herdr_request(herdr, target, args, 4, 1, NULL, err, sizeof err);
    CHECK(rc == VOICE_OK);
    atomic_int cancelled = 1;
    rc = voice_herdr_submit_guarded(herdr, target, &cancelled, 1, err, sizeof err);
    CHECK(rc == VOICE_REJECTED || rc == VOICE_CANCELLED);
    voice_herdr_free(herdr);
    voice_target_free(target);
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = 0;
        rmdir(path);
    }
}

static char seen_target[128];
static int saw_capture;

static void *fake_capture(const char *path, const char *target, void *user) {
    (void)path;
    (void)user;
    saw_capture = 1;
    snprintf(seen_target, sizeof seen_target, "%s", target ? target : "");
    return (void *)(intptr_t)1;
}

static int dump_runner(char *const *argv, double timeout, MicRunResult *out, void *user) {
    (void)argv;
    (void)timeout;
    (void)user;
    memset(out, 0, sizeof *out);
    out->ok = 1;
    out->stdout_text = strdup(
        "[{\"info\":{\"props\":{\"media.class\":\"Audio/Source\",\"node.name\":\"webcam\",\"node.description\":\"Webcam\"},\"params\":{\"Props\":[{\"mute\":false}]}}},"
        "{\"props\":{\"metadata.name\":\"default\"},\"metadata\":[{\"key\":\"default.audio.source\",\"value\":{\"name\":\"webcam\"}}]}]");
    return out->stdout_text ? 0 : -1;
}

static int noop_runner(const char *engine, const char *command, double timeout, void *ctx, char *err, size_t err_cap) {
    (void)engine; (void)command; (void)timeout; (void)ctx; (void)err; (void)err_cap;
    return 0;
}
static int noop_ready(const char *engine, double timeout, void *ctx, char *err, size_t err_cap) {
    (void)engine; (void)timeout; (void)ctx; (void)err; (void)err_cap;
    return 1;
}

static void test_default_capture_when_preferred_missing(void) {
    test_name = "default_capture";
    char dir[] = "/tmp/vmicXXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    audio_config config;
    audio_config_init(&config);
    config.preferred_microphone = "missing-node";
    config.tts_enabled = 0;
    audio *audio = audio_new(dir, &config);
    engine_config engines;
    memset(&engines, 0, sizeof engines);
    engines.runner = noop_runner;
    engines.readiness = noop_ready;
    engine_manager *manager = engine_manager_create(&engines);
    char err[256];
    voice_audio_binding *binding = voice_audio_bind_with_runner(audio, manager, 0, dump_runner, NULL, err, sizeof err);
    CHECK(binding != NULL);
    audio_set_capture(audio, fake_capture, NULL);
    saw_capture = 0;
    seen_target[0] = 'x';
    void *handle = audio_start_capture(audio, "/tmp/vmicXXXXXX/in.wav", err, sizeof err);
    CHECK(handle != NULL);
    CHECK(saw_capture);
    CHECK(seen_target[0] == '\0');
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.microphone_name, "Webcam") == 0);
    CHECK(!report.microphone_has_target);
    CHECK(report.microphone_missing);
    CHECK(report.microphone_has_preferred);
    CHECK(strcmp(report.microphone_preferred, "missing-node") == 0);
    CHECK(report.microphone_muted_known && !report.microphone_muted);
    char *json = NULL;
    CHECK(audio_microphone_json(audio, &json) == 0);
    CHECK(json && strstr(json, "\"target\":null") && strstr(json, "\"missing\":true") && strstr(json, "\"muted\":false"));
    free(json);
    voice_audio_unbind(binding);
    engine_manager_free(manager);
    audio_free(audio);
    rmdir(dir);
}

static void test_config_and_pid_bounds(void) {
    test_name = "config_bounds";
    const char *text = "{\"stt_url\":\"http://127.0.0.1:8178/inference\",\"tts_enabled\":false,\"auto_speak\":false}";
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    char err[128];
    voice_config *config = voice_config_parse(doc, err, sizeof err);
    CHECK(config);
    CHECK(voice_config_audio(config)->tts_enabled == 0);
    CHECK(voice_config_auto_speak(config) == 0);
    voice_config_free(config);
    yyjson_doc_free(doc);
    text = "{\"local_deepgram_api\":true,\"stt_backend\":\"deepgram\",\"speech_backend\":\"local\"}";
    doc = yyjson_read(text, strlen(text), 0);
    config = voice_config_parse(doc, err, sizeof err);
    CHECK(config);
    CHECK(voice_config_audio(config)->local_deepgram_api == 1);
    CHECK(strcmp(voice_config_audio(config)->stt_backend, "deepgram") == 0);
    CHECK(strcmp(voice_config_audio(config)->speech_backend, "local") == 0);
    voice_config_free(config);
    yyjson_doc_free(doc);
    text = "{\"stt_backend\":\"unknown\"}";
    doc = yyjson_read(text, strlen(text), 0);
    CHECK(voice_config_parse(doc, err, sizeof err) == NULL);
    yyjson_doc_free(doc);
    doc = yyjson_read("{\"pid\":999999999999}", 20, 0);
    voice_target *target = voice_target_from_doc(doc, err, sizeof err);
    CHECK(target == NULL);
    CHECK(strstr(err, "unavailable"));
    yyjson_doc_free(doc);
    char long_id[4100];
    memset(long_id, 'a', 4095);
    long_id[4095] = 0;
    char json[4300];
    snprintf(json, sizeof json, "{\"pane\":\"%s\"}", long_id);
    doc = yyjson_read(json, strlen(json), 0);
    target = voice_target_from_doc(doc, err, sizeof err);
    CHECK(target != NULL);
    CHECK(strcmp(voice_target_get_fields(target)->pane, long_id) == 0);
    voice_target_free(target);
    yyjson_doc_free(doc);
}

int test_runtime_adapters(void) {
    failures = 0;
    test_pi_lost_ack_is_uncertain_and_not_retried();
    test_herdr_input_has_empty_keys();
    test_config_and_pid_bounds();
    test_default_capture_when_preferred_missing();
    return failures;
}
