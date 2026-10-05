#define _POSIX_C_SOURCE 200809L
#include "ipc.h"

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

static void test_process_captures_without_shell(void) {
    test_name = "process_no_shell";
    const char *argv[] = {"echo", "hello adapter"};
    ipc_process_request request = {.argv = argv, .argc = 2, .capture_stdout = 1, .deadline_ms = 2000};
    ipc_process_result result;
    char err[128];
    CHECK(ipc_process_run(&request, &result, err, sizeof err) == IPC_OK);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_bytes && strstr((char *)result.stdout_bytes, "hello adapter"));
    ipc_process_result_free(&result);
    request.argc = 0;
    CHECK(ipc_process_run(&request, &result, err, sizeof err) == IPC_INVALID);
}

static void test_process_deadline_kills(void) {
    test_name = "process_deadline";
    const char *argv[] = {"sleep", "30"};
    ipc_process_request request = {.argv = argv, .argc = 2, .deadline_ms = 200, .capture_stdout = 1};
    ipc_process_result result;
    char err[128];
    int rc = ipc_process_run(&request, &result, err, sizeof err);
    CHECK(rc == IPC_TIMEOUT);
    CHECK(strstr(err, "deadline"));
    ipc_process_result_free(&result);
}

static void test_unix_json_roundtrip(void) {
    test_name = "unix_json";
    char dir[] = "/tmp/vipcXXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[64];
    snprintf(path, sizeof path, "%s/api.sock", dir);
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    CHECK(bind(server, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(server, 1) == 0);
    chmod(path, 0600);
    yyjson_doc *request = yyjson_read("{\"id\":\"abc\",\"method\":\"pane.send_input\"}", strlen("{\"id\":\"abc\",\"method\":\"pane.send_input\"}"), 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int conn = accept(server, NULL, NULL);
        char buf[256];
        size_t n = 0;
        while (n < sizeof buf - 1) {
            ssize_t got = read(conn, buf + n, 1);
            if (got <= 0) break;
            if (buf[n] == '\n') break;
            n += (size_t)got;
        }
        buf[n] = 0;
        const char *reply = "{\"id\":\"abc\",\"ok\":true}\n";
        if (strstr(buf, "pane.send_input") && write(conn, reply, strlen(reply)) < 0) _exit(1);
        _exit(0);
    }
    ipc_unix_request call = {.socket_path = path, .request = request, .deadline_ms = 2000, .max_response = 4096};
    int sent = 0;
    yyjson_doc *response = NULL;
    char err[128];
    CHECK(ipc_unix_json(&call, &sent, &response, err, sizeof err) == IPC_OK);
    CHECK(sent == 1);
    CHECK(response && yyjson_is_obj(yyjson_doc_get_root(response)));
    yyjson_doc_free(response);
    yyjson_doc_free(request);
    close(server);
    unlink(path);
    rmdir(dir);
}

int test_ipc(void) {
    failures = 0;
    test_process_captures_without_shell();
    test_process_deadline_kills();
    test_unix_json_roundtrip();
    return failures;
}
