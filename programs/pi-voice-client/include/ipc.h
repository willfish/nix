#ifndef PI_VOICE_IPC_H
#define PI_VOICE_IPC_H

#include <stddef.h>
#include <yyjson.h>

/* Bounded local IPC. No shell, no implicit retry, no secret logging. */

enum {
    IPC_OK = 0,
    IPC_ERR = -1,
    IPC_TIMEOUT = -2,
    IPC_TRUNCATED = -3,
    IPC_INVALID = -4,
    IPC_CANCELLED = -5
};

typedef struct ipc_env_override {
    const char *key;
    const char *value;
} ipc_env_override;

typedef struct ipc_process_request {
    const char *const *argv; /* argc entries, not necessarily NULL-terminated */
    int argc;
    const unsigned char *stdin_bytes; /* optional */
    size_t stdin_len;
    int capture_stdout;
    int capture_stderr;
    int deadline_ms; /* must be > 0 and <= 120000 */
    size_t stdout_max; /* 0 selects 1 MiB when capturing */
    size_t stderr_max; /* 0 selects 64 KiB when capturing */
    const ipc_env_override *env; /* overrides only; inherited environ otherwise */
    size_t env_count;
} ipc_process_request;

typedef struct ipc_process_result {
    int exit_code; /* -1 if the process did not exit */
    int timed_out;
    unsigned char *stdout_bytes; /* owned; free with ipc_process_result_free */
    size_t stdout_len;
    unsigned char *stderr_bytes;
    size_t stderr_len;
} ipc_process_result;

/* 0 and result filled. Nonzero: result may still hold partial output. */
int ipc_process_run(const ipc_process_request *request, ipc_process_result *result, char *err, size_t err_cap);
void ipc_process_result_free(ipc_process_result *result);

typedef struct ipc_unix_request {
    const char *socket_path;
    const yyjson_doc *request; /* immutable; not consumed; one JSON object */
    int deadline_ms; /* must be > 0 and <= 120000 */
    size_t max_response; /* 0 selects 1 MiB; a line above the cap is truncated */
} ipc_unix_request;

/* Writes one JSON line and reads one JSON line.
 * *sent is 1 after the first request byte is written, else 0.
 * *response is an owned immutable doc on IPC_OK, otherwise NULL.
 */
int ipc_unix_json(
    const ipc_unix_request *request, int *sent, yyjson_doc **response,
    char *err, size_t err_cap);

int test_ipc(void);

#endif
