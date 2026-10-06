#define _POSIX_C_SOURCE 200809L
#include "ipc.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
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

static volatile sig_atomic_t pipe_signals;
static void pipe_handler(int signal) { (void)signal; pipe_signals++; }

static void test_pipe_signal_ownership(void) {
    test_name = "pipe signal ownership";
    sigset_t block, old_mask, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    struct sigaction action = {.sa_handler = pipe_handler}, old_action, current;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGPIPE, &action, &old_action) == 0);
    CHECK(pthread_sigmask(SIG_BLOCK, &block, &old_mask) == 0);
    CHECK(raise(SIGPIPE) == 0);
    const char *argv[] = {"cat"};
    ipc_process_request request = {.argv = argv, .argc = 1,
        .stdin_bytes = (const unsigned char *)"payload", .stdin_len = 7,
        .capture_stdout = 1, .deadline_ms = 2000};
    ipc_process_result result;
    char err[128];
    CHECK(ipc_process_run(&request, &result, err, sizeof err) == IPC_OK);
    ipc_process_result_free(&result);
    CHECK(sigpending(&pending) == 0);
    CHECK(sigismember(&pending, SIGPIPE) == 1);
    CHECK(sigaction(SIGPIPE, NULL, &current) == 0);
    CHECK(current.sa_handler == pipe_handler);
    if (sigismember(&pending, SIGPIPE) == 1) {
        int signal;
        CHECK(sigwait(&block, &signal) == 0);
        CHECK(signal == SIGPIPE);
    }
    CHECK(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);

    /* A closed child pipe generates a new SIGPIPE, which belongs to the write. */
    const char *closed[] = {"sh", "-c", "exec 0<&-; sleep 1"};
    unsigned char *large = calloc(1, 1024 * 1024);
    CHECK(large != NULL);
    if (large) {
        request.argv = closed; request.argc = 3;
        request.stdin_bytes = large; request.stdin_len = 1024 * 1024;
        pipe_signals = 0;
        CHECK(ipc_process_run(&request, &result, err, sizeof err) == IPC_ERR);
        CHECK(pipe_signals == 0);
        CHECK(sigaction(SIGPIPE, NULL, &current) == 0);
        CHECK(current.sa_handler == pipe_handler);
        ipc_process_result_free(&result);
        free(large);
    }
    CHECK(sigaction(SIGPIPE, &old_action, NULL) == 0);
}

int test_ipc(void) {
    failures = 0;
    test_process_captures_without_shell();
    test_process_deadline_kills();
    test_unix_json_roundtrip();
    test_pipe_signal_ownership();
    return failures;
}
