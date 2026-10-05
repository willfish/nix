#define _POSIX_C_SOURCE 200809L
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum { IPC_ARGV_MAX = 32, IPC_ENV_MAX = 32, IPC_STDOUT_DEFAULT = 1024 * 1024, IPC_STDERR_DEFAULT = 64 * 1024 };

static void set_err(char *err, size_t cap, const char *msg) {
    if (!err || !cap) return;
    snprintf(err, cap, "%s", msg ? msg : "");
}

static int deadline_ok(int deadline_ms) {
    return deadline_ms > 0 && deadline_ms <= 120000;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static ssize_t write_nosig(int fd, const void *data, size_t len) {
    struct sigaction ignore, previous;
    memset(&ignore, 0, sizeof ignore);
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ignore, &previous);
    ssize_t n = write(fd, data, len);
    int saved = errno;
    sigaction(SIGPIPE, &previous, NULL);
    errno = saved;
    return n;
}

static int write_all_deadline(int fd, const unsigned char *data, size_t len, long long end_ms, size_t *written) {
    size_t off = 0;
    if (written) *written = 0;
    while (off < len) {
        long long left = end_ms - now_ms();
        if (left <= 0) return IPC_TIMEOUT;
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr = poll(&pfd, 1, left > 1000 ? 1000 : (int)left);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return IPC_ERR;
        }
        if (pr == 0) continue;
        ssize_t n = write_nosig(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return IPC_ERR;
        }
        off += (size_t)n;
        if (written) *written = off;
    }
    return IPC_OK;
}

static int read_bounded(int fd, unsigned char **out, size_t *out_len, size_t cap, long long end_ms) {
    size_t len = 0;
    size_t alloc = cap < 4096 ? cap : 4096;
    if (alloc == 0) alloc = 1;
    unsigned char *buf = malloc(alloc);
    if (!buf) return IPC_ERR;
    for (;;) {
        long long left = end_ms - now_ms();
        if (left <= 0) {
            free(buf);
            return IPC_TIMEOUT;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, left > 200 ? 200 : (int)left);
        if (pr < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return IPC_ERR;
        }
        if (pr == 0) continue;
        if (len >= cap) {
            unsigned char sink[512];
            ssize_t extra = read(fd, sink, sizeof sink);
            if (extra > 0) {
                free(buf);
                return IPC_TRUNCATED;
            }
            if (extra == 0) break;
            if (errno == EINTR || errno == EAGAIN) continue;
            free(buf);
            return IPC_ERR;
        }
        size_t room = cap - len;
        size_t chunk = room > 4096 ? 4096 : room;
        if (len + chunk > alloc) {
            size_t next = alloc * 2;
            if (next < len + chunk) next = len + chunk;
            if (next > cap) next = cap;
            unsigned char *grown = realloc(buf, next);
            if (!grown) {
                free(buf);
                return IPC_ERR;
            }
            buf = grown;
            alloc = next;
            if (len + chunk > alloc) chunk = alloc - len;
        }
        ssize_t n = read(fd, buf + len, chunk);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            free(buf);
            return IPC_ERR;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    *out = buf;
    *out_len = len;
    return IPC_OK;
}

static int env_key_eq(const char *entry, const char *key) {
    size_t n = strlen(key);
    return strncmp(entry, key, n) == 0 && entry[n] == '=';
}

static char **build_env(const ipc_env_override *env, size_t env_count, char *err, size_t err_cap) {
    if (env_count > IPC_ENV_MAX) {
        set_err(err, err_cap, "too many environment overrides");
        return NULL;
    }
    size_t base = 0;
    while (environ && environ[base]) base++;
    char **copy = calloc(base + env_count + 1, sizeof *copy);
    char **owned = calloc(env_count + 1, sizeof *owned);
    if (!copy || !owned) {
        free(copy);
        free(owned);
        set_err(err, err_cap, "out of memory");
        return NULL;
    }
    size_t n = 0;
    for (size_t i = 0; i < base; i++) {
        int replaced = 0;
        for (size_t e = 0; e < env_count; e++) {
            if (env[e].key && env_key_eq(environ[i], env[e].key)) replaced = 1;
        }
        if (!replaced) copy[n++] = environ[i];
    }
    for (size_t e = 0; e < env_count; e++) {
        if (!env[e].key || !env[e].key[0] || strchr(env[e].key, '=') || !env[e].value) {
            for (size_t i = 0; i < e; i++) free(owned[i]);
            free(owned);
            free(copy);
            set_err(err, err_cap, "invalid environment override");
            return NULL;
        }
        size_t len = strlen(env[e].key) + strlen(env[e].value) + 2;
        owned[e] = malloc(len);
        if (!owned[e]) {
            for (size_t i = 0; i < e; i++) free(owned[i]);
            free(owned);
            free(copy);
            set_err(err, err_cap, "out of memory");
            return NULL;
        }
        snprintf(owned[e], len, "%s=%s", env[e].key, env[e].value);
        copy[n++] = owned[e];
    }
    copy[n] = NULL;
    /* Stash the owned block in the unused slot after NULL by keeping owned[0] reachable
     * through a side channel: the first override string is freed by scanning copy. */
    free(owned);
    return copy;
}

static void free_env(char **env, const ipc_env_override *overrides, size_t env_count) {
    if (!env) return;
    for (char **p = env; *p; p++) {
        for (size_t i = 0; i < env_count; i++) {
            if (overrides[i].key && env_key_eq(*p, overrides[i].key)) {
                free(*p);
                break;
            }
        }
    }
    free(env);
}

void ipc_process_result_free(ipc_process_result *result) {
    if (!result) return;
    free(result->stdout_bytes);
    free(result->stderr_bytes);
    result->stdout_bytes = NULL;
    result->stderr_bytes = NULL;
    result->stdout_len = 0;
    result->stderr_len = 0;
}

int ipc_process_run(const ipc_process_request *request, ipc_process_result *result, char *err, size_t err_cap) {
    if (result) memset(result, 0, sizeof *result);
    if (result) result->exit_code = -1;
    if (!request || !result || request->argc < 1 || request->argc > IPC_ARGV_MAX || !request->argv) {
        set_err(err, err_cap, "invalid process request");
        return IPC_INVALID;
    }
    if (!deadline_ok(request->deadline_ms)) {
        set_err(err, err_cap, "invalid process deadline");
        return IPC_INVALID;
    }
    for (int i = 0; i < request->argc; i++) {
        if (!request->argv[i] || !request->argv[i][0]) {
            set_err(err, err_cap, "invalid process argv");
            return IPC_INVALID;
        }
    }
    size_t stdout_max = request->stdout_max ? request->stdout_max : IPC_STDOUT_DEFAULT;
    size_t stderr_max = request->stderr_max ? request->stderr_max : IPC_STDERR_DEFAULT;
    if (stdout_max > 8u * 1024u * 1024u || stderr_max > 1024u * 1024u) {
        set_err(err, err_cap, "process output cap is too large");
        return IPC_INVALID;
    }

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        set_err(err, err_cap, "could not create process pipes");
        goto fail_pipes;
    }
    for (int i = 0; i < 2; i++) {
        set_cloexec(in_pipe[i]);
        set_cloexec(out_pipe[i]);
        set_cloexec(err_pipe[i]);
        int flags = fcntl(in_pipe[i], F_GETFL);
        fcntl(in_pipe[i], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(out_pipe[i], F_GETFL);
        fcntl(out_pipe[i], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(err_pipe[i], F_GETFL);
        fcntl(err_pipe[i], F_SETFL, flags | O_NONBLOCK);
    }

    char **child_env = build_env(request->env, request->env_count, err, err_cap);
    if (!child_env) goto fail_pipes;

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    pid_t pid = 0;
    int actions_ok = posix_spawn_file_actions_init(&actions) == 0;
    int attr_ok = posix_spawnattr_init(&attr) == 0;
    if (!actions_ok || !attr_ok) {
        if (actions_ok) posix_spawn_file_actions_destroy(&actions);
        if (attr_ok) posix_spawnattr_destroy(&attr);
        set_err(err, err_cap, "could not prepare process spawn");
        free_env(child_env, request->env, request->env_count);
        goto fail_pipes;
    }
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    char *argv[IPC_ARGV_MAX + 1];
    for (int i = 0; i < request->argc; i++) argv[i] = (char *)request->argv[i];
    argv[request->argc] = NULL;
    int spawned = posix_spawnp(&pid, argv[0], &actions, &attr, argv, child_env);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    free_env(child_env, request->env, request->env_count);
    if (spawned != 0) {
        set_err(err, err_cap, "could not spawn process");
        goto fail_pipes;
    }
    close(in_pipe[0]);
    in_pipe[0] = -1;
    close(out_pipe[1]);
    out_pipe[1] = -1;
    close(err_pipe[1]);
    err_pipe[1] = -1;

    long long end_ms = now_ms() + request->deadline_ms;
    int write_rc = IPC_OK;
    if (request->stdin_bytes && request->stdin_len) {
        write_rc = write_all_deadline(in_pipe[1], request->stdin_bytes, request->stdin_len, end_ms, NULL);
    }
    close(in_pipe[1]);
    in_pipe[1] = -1;
    if (write_rc != IPC_OK) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        set_err(err, err_cap, write_rc == IPC_TIMEOUT ? "process deadline exceeded" : "could not write process input");
        goto fail_pipes;
    }

    unsigned char *stdout_buf = NULL, *stderr_buf = NULL;
    size_t stdout_len = 0, stderr_len = 0;
    int out_rc = IPC_OK, err_rc = IPC_OK;
    if (request->capture_stdout) out_rc = read_bounded(out_pipe[0], &stdout_buf, &stdout_len, stdout_max, end_ms);
    if (request->capture_stderr && out_rc == IPC_OK)
        err_rc = read_bounded(err_pipe[0], &stderr_buf, &stderr_len, stderr_max, end_ms);
    int status = 0;
    int timed_out = out_rc == IPC_TIMEOUT || err_rc == IPC_TIMEOUT;
    if (timed_out || out_rc == IPC_TRUNCATED || err_rc == IPC_TRUNCATED) kill(-pid, SIGKILL);
    for (;;) {
        pid_t got = waitpid(pid, &status, 0);
        if (got == pid) break;
        if (got < 0 && errno != EINTR) break;
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    if (stdout_buf) {
        unsigned char *z = realloc(stdout_buf, stdout_len + 1);
        if (z) {
            z[stdout_len] = 0;
            stdout_buf = z;
        }
    }
    if (stderr_buf) {
        unsigned char *z = realloc(stderr_buf, stderr_len + 1);
        if (z) {
            z[stderr_len] = 0;
            stderr_buf = z;
        }
    }
    result->stdout_bytes = stdout_buf;
    result->stdout_len = stdout_len;
    result->stderr_bytes = stderr_buf;
    result->stderr_len = stderr_len;
    result->timed_out = timed_out;
    if (timed_out) {
        set_err(err, err_cap, "process deadline exceeded");
        return IPC_TIMEOUT;
    }
    if (out_rc == IPC_TRUNCATED || err_rc == IPC_TRUNCATED) {
        set_err(err, err_cap, "process output exceeded the cap");
        return IPC_TRUNCATED;
    }
    if (out_rc != IPC_OK || err_rc != IPC_OK) {
        set_err(err, err_cap, "could not read process output");
        return IPC_ERR;
    }
    if (WIFEXITED(status)) result->exit_code = WEXITSTATUS(status);
    else result->exit_code = -1;
    return IPC_OK;

fail_pipes:
    if (in_pipe[0] >= 0) close(in_pipe[0]);
    if (in_pipe[1] >= 0) close(in_pipe[1]);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    return err && err[0] ? IPC_ERR : IPC_ERR;
}

static int same_user_socket(const char *path, char *err, size_t err_cap) {
    struct stat st;
    if (!path || !path[0] || path[0] != '/' || lstat(path, &st) != 0) {
        set_err(err, err_cap, "Unix socket is unavailable");
        return -1;
    }
    if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
        set_err(err, err_cap, "Unix socket is not a same-user socket");
        return -1;
    }
    return 0;
}

int ipc_unix_json(
    const ipc_unix_request *request, int *sent, yyjson_doc **response, char *err, size_t err_cap
) {
    if (sent) *sent = 0;
    if (response) *response = NULL;
    if (!request || !response || !request->socket_path || !request->request || !deadline_ok(request->deadline_ms)) {
        set_err(err, err_cap, "invalid Unix JSON request");
        return IPC_INVALID;
    }
    size_t cap = request->max_response ? request->max_response : IPC_STDOUT_DEFAULT;
    if (cap > 8u * 1024u * 1024u) {
        set_err(err, err_cap, "Unix response cap is too large");
        return IPC_INVALID;
    }
    yyjson_val *root = yyjson_doc_get_root((yyjson_doc *)request->request);
    if (!root || !yyjson_is_obj(root)) {
        set_err(err, err_cap, "Unix JSON request must be an object");
        return IPC_INVALID;
    }
    if (same_user_socket(request->socket_path, err, err_cap) != 0) return IPC_ERR;

    size_t json_len = 0;
    yyjson_write_err write_err;
    memset(&write_err, 0, sizeof write_err);
    char *payload = yyjson_write_opts(request->request, 0, NULL, &json_len, &write_err);
    if (!payload) {
        set_err(err, err_cap, "could not encode Unix JSON request");
        return IPC_ERR;
    }
    char *line = malloc(json_len + 2);
    if (!line) {
        free(payload);
        set_err(err, err_cap, "out of memory");
        return IPC_ERR;
    }
    memcpy(line, payload, json_len);
    line[json_len] = '\n';
    line[json_len + 1] = '\0';
    free(payload);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        free(line);
        set_err(err, err_cap, "could not open a Unix socket");
        return IPC_ERR;
    }
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(request->socket_path) >= sizeof addr.sun_path) {
        close(fd);
        free(line);
        set_err(err, err_cap, "Unix socket path is too long");
        return IPC_INVALID;
    }
    memcpy(addr.sun_path, request->socket_path, strlen(request->socket_path) + 1);
    long long end_ms = now_ms() + request->deadline_ms;
    int connected = 0;
    for (;;) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            connected = 1;
            break;
        }
        if (errno == EISCONN) {
            connected = 1;
            break;
        }
        if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR) break;
        long long left = end_ms - now_ms();
        if (left <= 0) break;
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr = poll(&pfd, 1, left > 200 ? 200 : (int)left);
        if (pr < 0 && errno != EINTR) break;
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0 && pr > 0) {
            connected = 1;
            break;
        }
        if (soerr && soerr != EINPROGRESS) break;
    }
    if (!connected) {
        close(fd);
        free(line);
        set_err(err, err_cap, "Unix socket is unavailable");
        return IPC_ERR;
    }
    size_t written = 0;
    int write_rc = write_all_deadline(fd, (unsigned char *)line, json_len + 1, end_ms, &written);
    free(line);
    if (write_rc != IPC_OK) {
        if (sent) *sent = written > 0;
        close(fd);
        set_err(err, err_cap, write_rc == IPC_TIMEOUT ? "Unix write deadline exceeded" : "Unix write failed");
        return write_rc == IPC_TIMEOUT ? IPC_TIMEOUT : IPC_ERR;
    }
    if (sent) *sent = 1;

    unsigned char *buf = malloc(cap + 1);
    if (!buf) {
        close(fd);
        set_err(err, err_cap, "out of memory");
        return IPC_ERR;
    }
    size_t len = 0;
    int saw_nl = 0;
    while (len < cap) {
        long long left = end_ms - now_ms();
        if (left <= 0) {
            free(buf);
            close(fd);
            set_err(err, err_cap, "Unix read deadline exceeded");
            return IPC_TIMEOUT;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, left > 200 ? 200 : (int)left);
        if (pr < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            set_err(err, err_cap, "Unix read failed");
            return IPC_ERR;
        }
        if (pr == 0) continue;
        ssize_t n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            free(buf);
            close(fd);
            set_err(err, err_cap, "Unix read failed");
            return IPC_ERR;
        }
        if (n == 0) break;
        len += (size_t)n;
        if (memchr(buf, '\n', len)) {
            saw_nl = 1;
            break;
        }
    }
    close(fd);
    if (!saw_nl) {
        int overflow = len >= cap;
        free(buf);
        set_err(err, err_cap, overflow ? "Unix response exceeded the cap" : "Unix response was incomplete");
        return overflow ? IPC_TRUNCATED : IPC_ERR;
    }
    size_t line_len = (size_t)((unsigned char *)memchr(buf, '\n', len) - buf);
    buf[line_len] = '\0';
    yyjson_doc *doc = yyjson_read((char *)buf, line_len, 0);
    free(buf);
    if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
        yyjson_doc_free(doc);
        set_err(err, err_cap, "Unix response was not a JSON object");
        return IPC_ERR;
    }
    *response = doc;
    return IPC_OK;
}
