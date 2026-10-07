#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ignore_pipe(int sig) { (void)sig; }

static char *xdup(const char *s)
{
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) exit(1);
    memcpy(p, s, n + 1);
    return p;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int append_buf(char **buf, size_t *n, size_t *cap, const char *data, size_t len)
{
    if (*n + len + 1 > *cap) {
        size_t next = *cap ? *cap * 2 : 256;
        char *grown;
        while (next < *n + len + 1) next *= 2;
        grown = realloc(*buf, next);
        if (!grown) return -1;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *n, data, len);
    *n += len;
    (*buf)[*n] = '\0';
    return 0;
}

static char *json_escape_array(char **items, int count)
{
    char *out = NULL;
    size_t n = 0, cap = 0;
    int i;
    if (append_buf(&out, &n, &cap, "[", 1)) return NULL;
    for (i = 0; i < count; i++) {
        const unsigned char *p;
        if (i && append_buf(&out, &n, &cap, ",", 1)) { free(out); return NULL; }
        if (append_buf(&out, &n, &cap, "\"", 1)) { free(out); return NULL; }
        for (p = (const unsigned char *)items[i]; *p; p++) {
            char esc[8];
            if (*p == '"' || *p == '\\') snprintf(esc, sizeof esc, "\\%c", *p);
            else if (*p < 32) snprintf(esc, sizeof esc, "\\u%04x", *p);
            else { esc[0] = (char)*p; esc[1] = 0; }
            if (append_buf(&out, &n, &cap, esc, strlen(esc))) { free(out); return NULL; }
        }
        if (append_buf(&out, &n, &cap, "\"", 1)) { free(out); return NULL; }
    }
    if (append_buf(&out, &n, &cap, "]", 1)) { free(out); return NULL; }
    return out;
}

static void log_call(const char *logged, int argc, char **argv)
{
    const char *path = getenv("VOICE_CALLS");
    char **items;
    char *line;
    int fd, i;
    if (!path || !path[0] || !logged) return;
    items = calloc((size_t)argc, sizeof *items);
    if (!items) return;
    items[0] = (char *)logged;
    for (i = 1; i < argc; i++) items[i] = argv[i];
    line = json_escape_array(items, argc);
    free(items);
    if (!line) return;
    fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd >= 0) {
        write_all(fd, line, strlen(line));
        write_all(fd, "\n", 1);
        close(fd);
    }
    free(line);
}

static void tool_exit(int sig)
{
    (void)sig;
    _exit(0);
}

static int tool_record(void)
{
    struct sigaction action = {0};
    int frame;
    action.sa_handler = tool_exit;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    for (frame = 0; frame < 500; frame++) {
        int voiced = 0, i, start;
        unsigned char pcm[640];
        struct timespec delay = {.tv_nsec = 20000000};
        for (start = 0; start <= 180; start += 60)
            if (start <= frame && frame < start + 20) voiced = 1;
        for (i = 0; i < 320; i++) {
            int sample = voiced ? (int)trunc(1500.0 * sin(i * 0.17)) : 0;
            pcm[i * 2] = (unsigned char)(sample & 255);
            pcm[i * 2 + 1] = (unsigned char)((sample >> 8) & 255);
        }
        if (write_all(STDOUT_FILENO, pcm, sizeof pcm) != 0) {
            if (errno == EPIPE) break;
            return 1;
        }
        nanosleep(&delay, NULL);
    }
    return 0;
}

static int tool_play(void)
{
    char buf[4096];
    while (read(STDIN_FILENO, buf, sizeof buf) > 0) {}
    return 0;
}

static int tool_pi(int argc, char **argv)
{
    const char *path = getenv("VOICE_PI_RESULT");
    const char *token = getenv("AGENT_VOICE_TOKEN");
    const char *kind = getenv("AGENT_VOICE_KIND");
    char **args;
    char *encoded, *text;
    FILE *fp;
    int i;
    if (!path) return 1;
    args = calloc((size_t)(argc > 0 ? argc : 1), sizeof *args);
    if (!args) return 1;
    for (i = 1; i < argc; i++) args[i - 1] = argv[i];
    encoded = json_escape_array(args, argc > 0 ? argc - 1 : 0);
    free(args);
    if (!encoded) return 1;
    text = malloc(strlen(encoded) + strlen(token ? token : "") + strlen(kind ? kind : "") + 64);
    if (!text) { free(encoded); return 1; }
    snprintf(text, strlen(encoded) + strlen(token ? token : "") + strlen(kind ? kind : "") + 64,
             "{\"args\":%s,\"token\":\"%s\",\"kind\":\"%s\"}", encoded, token ? token : "", kind ? kind : "");
    fp = fopen(path, "w");
    if (!fp) { free(text); free(encoded); return 1; }
    fputs(text, fp);
    fclose(fp);
    free(text);
    free(encoded);
    return 0;
}

static const char dump_json[] =
    "[{\"type\":\"PipeWire:Interface:Node\",\"info\":{\"props\":{\"node.name\":\"synthetic\","
    "\"node.description\":\"Synthetic microphone\",\"media.class\":\"Audio/Source\"},"
    "\"params\":{\"Props\":[{\"mute\":false}]}}},{\"props\":{\"metadata.name\":\"default\"},"
    "\"metadata\":[{\"key\":\"default.audio.source\",\"value\":{\"name\":\"synthetic\"}}]}]";

static int tool_main(const char *name, int argc, char **argv)
{
    const char *profile = getenv("VOICE_PROFILE");
    int journey = profile && strcmp(profile, "journey") == 0;
    int show = 0, i;
    char exe[PATH_MAX];
    ssize_t got;
    const char *base = strrchr(name, '/');
    const char *exe_base;
    const char *logged = name;
    signal(SIGPIPE, SIG_IGN);
    base = base ? base + 1 : name;
    got = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (got > 0) {
        exe[got] = '\0';
        exe_base = strrchr(exe, '/');
        exe_base = exe_base ? exe_base + 1 : exe;
        if (!strcmp(exe_base, base)) logged = exe;
    }
    log_call(logged, argc, argv);
    for (i = 1; i < argc; i++) if (!strcmp(argv[i], "show")) show = 1;
    if (!strcmp(base, "systemctl")) {
        if (show) fputs("inactive\n", stdout);
        return 0;
    }
    if (!strcmp(base, "pw-dump")) {
        fputs(journey ? dump_json : "[]", stdout);
        fputc('\n', stdout);
        return 0;
    }
    if (!strcmp(base, "pw-record")) return journey ? tool_record() : 97;
    if (!strcmp(base, "pw-play")) return journey ? tool_play() : 97;
    if (!strcmp(base, "herdr")) {
        const char *pid = getenv("VOICE_FIXTURE_PID");
        if (!journey) return 97;
        printf("{\"ok\":true,\"result\":{\"process_info\":{\"pane_id\":\"w1:p1\","
               "\"foreground_processes\":[{\"pid\":%s,\"name\":\"pi\"}]},\"snapshot\":{"
               "\"workspaces\":[{\"workspace_id\":\"w1\",\"name\":\"Native fixture\"}],"
               "\"tabs\":[{\"tab_id\":\"t1\",\"name\":\"Synthetic pane\"}],"
               "\"panes\":[{\"pane_id\":\"w1:p1\",\"workspace_id\":\"w1\",\"tab_id\":\"t1\"}]}}}\n",
               pid && *pid ? pid : "0");
        return 0;
    }
    if (!strcmp(base, "pi")) return tool_pi(argc, argv);
    return 97;
}

typedef struct {
    pid_t pid;
    int out_fd, err_fd;
    pthread_t reader;
    pthread_mutex_t mu;
    char *out, *err;
    size_t out_n, err_n, out_cap, err_cap;
    int done, code, signal;
} proc;

static void proc_append(proc *p, int err, const char *data, size_t n)
{
    pthread_mutex_lock(&p->mu);
    if (err) append_buf(&p->err, &p->err_n, &p->err_cap, data, n);
    else append_buf(&p->out, &p->out_n, &p->out_cap, data, n);
    pthread_mutex_unlock(&p->mu);
}

static void *proc_read(void *arg)
{
    proc *p = arg;
    while (p->out_fd >= 0 || p->err_fd >= 0) {
        struct pollfd fds[2];
        int nfds = 0, outi = -1, erri = -1;
        if (p->out_fd >= 0) { outi = nfds; fds[nfds].fd = p->out_fd; fds[nfds].events = POLLIN; nfds++; }
        if (p->err_fd >= 0) { erri = nfds; fds[nfds].fd = p->err_fd; fds[nfds].events = POLLIN; nfds++; }
        if (!nfds || poll(fds, (nfds_t)nfds, 200) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (outi >= 0 && (fds[outi].revents & (POLLIN | POLLHUP))) {
            char buf[1024];
            ssize_t n = read(p->out_fd, buf, sizeof buf);
            if (n > 0) proc_append(p, 0, buf, (size_t)n);
            else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) { close(p->out_fd); p->out_fd = -1; }
        }
        if (erri >= 0 && (fds[erri].revents & (POLLIN | POLLHUP))) {
            char buf[1024];
            ssize_t n = read(p->err_fd, buf, sizeof buf);
            if (n > 0) proc_append(p, 1, buf, (size_t)n);
            else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) { close(p->err_fd); p->err_fd = -1; }
        }
    }
    return NULL;
}

static int proc_start(proc *p, const char *binary, char **argv, char **envp)
{
    int outp[2], errp[2];
    memset(p, 0, sizeof *p);
    p->out_fd = p->err_fd = -1;
    p->code = -1;
    if (pipe(outp) != 0 || pipe(errp) != 0) return -1;
    p->pid = fork();
    if (p->pid < 0) return -1;
    if (!p->pid) {
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        execve(binary, argv, envp);
        _exit(127);
    }
    close(outp[1]);
    close(errp[1]);
    p->out_fd = outp[0];
    p->err_fd = errp[0];
    pthread_mutex_init(&p->mu, NULL);
    if (pthread_create(&p->reader, NULL, proc_read, p) != 0) return -1;
    return 0;
}

static void proc_finish(proc *p, int timeout_ms)
{
    int64_t end = now_ms() + timeout_ms;
    int status = 0;
    if (!p || p->pid <= 0) return;
    while (!p->done) {
        pid_t got = waitpid(p->pid, &status, WNOHANG);
        if (got == p->pid) {
            p->done = 1;
            if (WIFEXITED(status)) p->code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) p->signal = WTERMSIG(status);
            break;
        }
        if (now_ms() > end) {
            kill(p->pid, SIGKILL);
            waitpid(p->pid, &status, 0);
            p->done = 1;
            p->signal = SIGKILL;
            break;
        }
        usleep(20000);
    }
    pthread_join(p->reader, NULL);
    pthread_mutex_destroy(&p->mu);
    p->pid = -1;
}

static void proc_cleanup(proc *p)
{
    if (!p || p->pid <= 0) return;
    if (!p->done) kill(p->pid, SIGKILL);
    proc_finish(p, 1000);
}

static char *proc_stderr(proc *p)
{
    char *copy;
    if (!p) return xdup("");
    pthread_mutex_lock(&p->mu);
    copy = xdup(p->err ? p->err : "");
    pthread_mutex_unlock(&p->mu);
    return copy;
}

typedef struct {
    char home[PATH_MAX];
    char runtime[PATH_MAX];
    char tools[PATH_MAX];
    char config[PATH_MAX];
    char calls[PATH_MAX];
    char pi_result[PATH_MAX];
    char profile[16];
    int fixture_pid;
} env_spec;

static char **build_env(const env_spec *spec, const char *extra_key, const char *extra_val)
{
    char home[PATH_MAX + 8], runtime[PATH_MAX + 24], config[PATH_MAX + 24], path[PATH_MAX + 8];
    char calls[PATH_MAX + 16], profile[32], pid[64], result[PATH_MAX + 24], extra[PATH_MAX + 64];
    char **envp = calloc(12, sizeof *envp);
    int n = 0;
    if (!envp) return NULL;
    snprintf(home, sizeof home, "HOME=%s", spec->home);
    snprintf(runtime, sizeof runtime, "XDG_RUNTIME_DIR=%s", spec->runtime);
    snprintf(config, sizeof config, "PI_VOICE_CONFIG=%s", spec->config);
    snprintf(path, sizeof path, "PATH=%s", spec->tools);
    snprintf(calls, sizeof calls, "VOICE_CALLS=%s", spec->calls);
    snprintf(profile, sizeof profile, "VOICE_PROFILE=%s", spec->profile);
    snprintf(pid, sizeof pid, "VOICE_FIXTURE_PID=%d", spec->fixture_pid);
    envp[n++] = xdup(home);
    envp[n++] = xdup(runtime);
    envp[n++] = xdup(config);
    envp[n++] = xdup(path);
    envp[n++] = xdup("LANG=C.UTF-8");
    envp[n++] = xdup("LC_ALL=C.UTF-8");
    envp[n++] = xdup(calls);
    envp[n++] = xdup(profile);
    envp[n++] = xdup(pid);
    if (spec->pi_result[0]) {
        snprintf(result, sizeof result, "VOICE_PI_RESULT=%s", spec->pi_result);
        envp[n++] = xdup(result);
    }
    if (extra_key) {
        snprintf(extra, sizeof extra, "%s=%s", extra_key, extra_val ? extra_val : "");
        envp[n++] = xdup(extra);
    }
    return envp;
}

static void free_env(char **envp)
{
    int i;
    if (!envp) return;
    for (i = 0; envp[i]; i++) free(envp[i]);
    free(envp);
}

static int install_tools(const char *dir)
{
    char exe[PATH_MAX];
    const char *names[] = {"systemctl", "pw-dump", "pw-record", "pw-play", "herdr", "wl-copy", "pi"};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    size_t i;
    if (n < 0) return -1;
    exe[n] = '\0';
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        unlink(path);
        if (link(exe, path) != 0) {
            FILE *in = fopen(exe, "rb"), *out;
            char buf[8192];
            size_t got;
            if (!in) return -1;
            out = fopen(path, "wb");
            if (!out) { fclose(in); return -1; }
            while ((got = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, got, out);
            fclose(in);
            fclose(out);
            chmod(path, 0700);
        }
    }
    return 0;
}

static int mode_is(const char *path, mode_t mode)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & 0777) == mode;
}

static yyjson_doc *ipc_request(const char *path, const char *json, int timeout_ms, char *err, size_t cap)
{
    int fd, sent = 0;
    struct sockaddr_un addr;
    char *buf = NULL;
    size_t n = 0, size = 0;
    int64_t end = now_ms() + timeout_ms;
    yyjson_doc *doc = NULL;
    err[0] = '\0';
    if (strlen(path) >= sizeof addr.sun_path) {
        snprintf(err, cap, "socket path too long");
        return NULL;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { snprintf(err, cap, "socket"); return NULL; }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    while (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        if (errno == EINTR) continue;
        snprintf(err, cap, "connect");
        close(fd);
        return NULL;
    }
    if (write_all(fd, json, strlen(json)) != 0 || write_all(fd, "\n", 1) != 0) {
        snprintf(err, cap, "write");
        close(fd);
        return NULL;
    }
    sent = 1;
    while (now_ms() < end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 40);
        char tmp[1024];
        ssize_t got;
        char *nl;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        got = read(fd, tmp, sizeof tmp);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        if (append_buf(&buf, &n, &size, tmp, (size_t)got) != 0) break;
        if (n > 1024 * 1024) { snprintf(err, cap, "Oversized response"); break; }
        nl = memchr(buf, '\n', n);
        if (!nl) continue;
        *nl = '\0';
        doc = yyjson_read(buf, strlen(buf), 0);
        if (!doc) snprintf(err, cap, "invalid json");
        free(buf);
        close(fd);
        return doc;
    }
    free(buf);
    close(fd);
    if (!err[0]) snprintf(err, cap, sent ? "Socket request timed out" : "connect");
    return NULL;
}

typedef char *(*json_fn)(const char *text, void *user, int *failed);

typedef struct {
    int fd;
    pthread_t thread;
    json_fn fn;
    void *user;
    volatile int stop;
    int errors;
    pthread_mutex_t mu;
} json_srv;

static int read_line_fd(int fd, char **out, int timeout_ms)
{
    char *buf = NULL;
    size_t n = 0, cap = 0;
    int64_t end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        char tmp[1024];
        ssize_t got;
        char *nl;
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        got = read(fd, tmp, sizeof tmp);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        if (append_buf(&buf, &n, &cap, tmp, (size_t)got) != 0) break;
        if (n > 1024 * 1024) break;
        nl = memchr(buf, '\n', n);
        if (!nl) continue;
        *nl = '\0';
        *out = buf;
        return 0;
    }
    free(buf);
    return -1;
}

static void *json_thread(void *arg)
{
    json_srv *srv = arg;
    while (!srv->stop) {
        struct pollfd pfd = {.fd = srv->fd, .events = POLLIN};
        int client, failed = 0;
        char *line = NULL, *reply;
        if (poll(&pfd, 1, 100) <= 0) continue;
        client = accept(srv->fd, NULL, NULL);
        if (client < 0) continue;
        if (read_line_fd(client, &line, 3000) == 0) {
            reply = srv->fn(line, srv->user, &failed);
            if (failed) {
                pthread_mutex_lock(&srv->mu);
                srv->errors++;
                pthread_mutex_unlock(&srv->mu);
            } else if (reply) {
                write_all(client, reply, strlen(reply));
                write_all(client, "\n", 1);
            }
            free(reply);
        }
        free(line);
        close(client);
    }
    return NULL;
}

static int json_listen(json_srv *srv, const char *path, json_fn fn, void *user)
{
    struct sockaddr_un addr;
    memset(srv, 0, sizeof *srv);
    if (strlen(path) >= sizeof addr.sun_path) return -1;
    unlink(path);
    srv->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv->fd < 0) return -1;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (bind(srv->fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv->fd, 16) != 0) {
        close(srv->fd);
        return -1;
    }
    chmod(path, 0600);
    srv->fn = fn;
    srv->user = user;
    pthread_mutex_init(&srv->mu, NULL);
    if (pthread_create(&srv->thread, NULL, json_thread, srv) != 0) return -1;
    return 0;
}

static void json_close(json_srv *srv)
{
    if (!srv || srv->fd < 0) return;
    srv->stop = 1;
    shutdown(srv->fd, SHUT_RDWR);
    pthread_join(srv->thread, NULL);
    close(srv->fd);
    srv->fd = -1;
    pthread_mutex_destroy(&srv->mu);
    CHECK(srv->errors == 0);
}

typedef struct {
    pthread_mutex_t mu;
    char actions[16][64];
    char tokens[16][128];
    int count;
} received;

static char *smoke_handler(const char *text, void *user, int *failed)
{
    received *box = user;
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *action = root ? yyjson_get_str(yyjson_obj_get(root, "action")) : "";
    const char *token = root ? yyjson_get_str(yyjson_obj_get(root, "token")) : "";
    int interact;
    (void)failed;
    if (!action) action = "";
    if (!token) token = "";
    interact = strcmp(action, "interact") == 0;
    pthread_mutex_lock(&box->mu);
    if (box->count < 16) {
        snprintf(box->actions[box->count], sizeof box->actions[box->count], "%s", action);
        snprintf(box->tokens[box->count], sizeof box->tokens[box->count], "%s", token);
        box->count++;
    }
    pthread_mutex_unlock(&box->mu);
    yyjson_doc_free(doc);
    if (interact) return NULL;
    return xdup("{\"ok\":true}");
}

typedef struct {
    pthread_mutex_t mu;
    char token[128];
    char staged[8][256];
    int staged_n;
    char submitted[4][1024];
    int submitted_n;
    int armed;
    char text[1024];
    int pid;
    char session[64];
    char bridge[40];
    int activation;
    char harness[16];
} adapter_state;

static int same_identity(adapter_state *st, yyjson_val *root)
{
    yyjson_val *pid = yyjson_obj_get(root, "pid");
    yyjson_val *activation = yyjson_obj_get(root, "activation");
    const char *session = yyjson_get_str(yyjson_obj_get(root, "session"));
    const char *bridge = yyjson_get_str(yyjson_obj_get(root, "bridge_id"));
    const char *harness = yyjson_get_str(yyjson_obj_get(root, "harness"));
    const char *token = yyjson_get_str(yyjson_obj_get(root, "token"));
    if (!token || strcmp(token, st->token) != 0) return 0;
    if (!pid || !yyjson_is_int(pid) || yyjson_get_int(pid) != st->pid) return 0;
    if (!session || strcmp(session, st->session) != 0) return 0;
    if (!bridge || strcmp(bridge, st->bridge) != 0) return 0;
    if (!activation || !yyjson_is_int(activation) || yyjson_get_int(activation) != st->activation) return 0;
    if (!harness || strcmp(harness, st->harness) != 0) return 0;
    return 1;
}

static char *adapter_handler(const char *text, void *user, int *failed)
{
    adapter_state *st = user;
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *command = root ? yyjson_get_str(yyjson_obj_get(root, "command")) : "";
    char *reply = NULL;
    (void)failed;
    if (!command) command = "";
    pthread_mutex_lock(&st->mu);
    if (!root || !same_identity(st, root)) reply = xdup("{\"ok\":false,\"error\":\"fixture identity mismatch\"}");
    else if (!strcmp(command, "stage")) {
        const char *piece = yyjson_get_str(yyjson_obj_get(root, "text"));
        if (!piece) piece = "";
        if (st->text[0]) {
            size_t n = strlen(st->text);
            if (n + 1 < sizeof st->text) st->text[n++] = '\n', st->text[n] = '\0';
        }
        snprintf(st->text + strlen(st->text), sizeof st->text - strlen(st->text), "%s", piece);
        if (st->staged_n < 8) snprintf(st->staged[st->staged_n++], sizeof st->staged[0], "%s", piece);
        st->armed = 1;
        reply = xdup("{\"ok\":true,\"result\":{}}");
    } else if (!strcmp(command, "submit") && st->armed) {
        if (st->submitted_n < 4) snprintf(st->submitted[st->submitted_n++], sizeof st->submitted[0], "%s", st->text);
        st->armed = 0;
        reply = xdup("{\"ok\":true,\"result\":{}}");
    } else if (!strcmp(command, "status")) {
        char buf[512];
        snprintf(buf, sizeof buf,
                 "{\"ok\":true,\"result\":{\"pid\":%d,\"session\":\"%s\",\"bridge_id\":\"%s\","
                 "\"activation\":%d,\"harness\":\"%s\",\"ready\":true,\"accepts_input\":true,"
                 "\"state\":\"idle\",\"draft\":%s,\"draft_state\":\"%s\"}}",
                 st->pid, st->session, st->bridge, st->activation, st->harness,
                 st->armed ? "true" : "false", st->armed ? "staged" : "none");
        reply = xdup(buf);
    } else reply = xdup("{\"ok\":false,\"error\":\"no owned draft\"}");
    pthread_mutex_unlock(&st->mu);
    yyjson_doc_free(doc);
    return reply;
}

static char *ok_handler(const char *text, void *user, int *failed)
{
    (void)text; (void)user; (void)failed;
    return xdup("{\"ok\":true}");
}

typedef struct {
    int fd;
    pthread_t thread;
    volatile int stop;
    int posts;
    unsigned char *bodies[8];
    size_t body_n[8];
    pthread_mutex_t mu;
} http_srv;

static int header_value(const char *headers, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = headers;
    while (*p) {
        const char *nl = strstr(p, "\r\n");
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n > klen + 1 && strncasecmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            size_t vlen;
            while (v < p + n && *v == ' ') v++;
            vlen = (size_t)((p + n) - v);
            if (vlen + 1 > cap) return -1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 0;
        }
        if (!nl) break;
        p = nl + 2;
    }
    return -1;
}

static void *http_thread(void *arg)
{
    http_srv *srv = arg;
    while (!srv->stop) {
        struct pollfd pfd = {.fd = srv->fd, .events = POLLIN};
        int client;
        char header[4096];
        size_t n = 0;
        char *split;
        if (poll(&pfd, 1, 100) <= 0) continue;
        client = accept(srv->fd, NULL, NULL);
        if (client < 0) continue;
        while (n + 1 < sizeof header) {
            ssize_t got = read(client, header + n, 1);
            if (got <= 0) break;
            n += (size_t)got;
            header[n] = '\0';
            if (n >= 4 && strstr(header, "\r\n\r\n")) break;
        }
        split = strstr(header, "\r\n\r\n");
        if (split) {
            char expect[64] = {0}, length[32] = {0}, method[16] = {0};
            long body_len = 0;
            unsigned char *body = NULL;
            int number = 0;
            sscanf(header, "%15s", method);
            header_value(header, "Content-Length", length, sizeof length);
            header_value(header, "Expect", expect, sizeof expect);
            body_len = strtol(length, NULL, 10);
            if (strcasestr(expect, "100-continue"))
                write_all(client, "HTTP/1.1 100 Continue\r\n\r\n", 25);
            if (body_len > 0 && body_len < 8 * 1024 * 1024) {
                size_t have = 0;
                body = malloc((size_t)body_len);
                while (body && have < (size_t)body_len) {
                    ssize_t got = read(client, body + have, (size_t)body_len - have);
                    if (got <= 0) break;
                    have += (size_t)got;
                }
                if (have != (size_t)body_len) { free(body); body = NULL; }
            }
            if (!strcmp(method, "POST") && body) {
                pthread_mutex_lock(&srv->mu);
                number = ++srv->posts;
                if (number <= 8) {
                    srv->bodies[number - 1] = body;
                    srv->body_n[number - 1] = (size_t)body_len;
                    body = NULL;
                }
                pthread_mutex_unlock(&srv->mu);
                if (number == 1) {
                    struct timespec delay = {.tv_sec = 4, .tv_nsec = 200000000};
                    nanosleep(&delay, NULL);
                }
            }
            free(body);
            {
                char payload[128];
                char response[256];
                if (!strcmp(method, "POST"))
                    snprintf(payload, sizeof payload,
                             "{\"results\":{\"channels\":[{\"alternatives\":[{\"transcript\":\"Native phrase %d.\"}]}]}}",
                             number > 0 ? number : 1);
                else snprintf(payload, sizeof payload, "{\"status\":\"ok\"}");
                snprintf(response, sizeof response,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                         strlen(payload), payload);
                write_all(client, response, strlen(response));
            }
        }
        close(client);
    }
    return NULL;
}

static int http_listen(http_srv *srv, int *port)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    memset(srv, 0, sizeof *srv);
    srv->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv->fd < 0) return -1;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv->fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv->fd, 8) != 0) return -1;
    if (getsockname(srv->fd, (struct sockaddr *)&addr, &len) != 0) return -1;
    *port = ntohs(addr.sin_port);
    pthread_mutex_init(&srv->mu, NULL);
    if (pthread_create(&srv->thread, NULL, http_thread, srv) != 0) return -1;
    return 0;
}

static void http_close(http_srv *srv)
{
    int i;
    if (!srv || srv->fd < 0) return;
    srv->stop = 1;
    shutdown(srv->fd, SHUT_RDWR);
    pthread_join(srv->thread, NULL);
    close(srv->fd);
    srv->fd = -1;
    for (i = 0; i < 8; i++) free(srv->bodies[i]);
    pthread_mutex_destroy(&srv->mu);
}

static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;
    if (!dir) { unlink(path); return; }
    while ((ent = readdir(dir)) != NULL) {
        char child[PATH_MAX];
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
        remove_tree(child);
    }
    closedir(dir);
    rmdir(path);
}

static int write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fputs(text, fp);
    return fclose(fp);
}

static int join_path(char *out, size_t cap, const char *dir, const char *leaf)
{
    size_t nd = strlen(dir), nl = strlen(leaf);
    if (nd + 1 + nl + 1 > cap) return -1;
    memcpy(out, dir, nd);
    out[nd] = '/';
    memcpy(out + nd + 1, leaf, nl + 1);
    return 0;
}

static int prepare(env_spec *spec, const char *prefix, const char *config_json, const char *profile)
{
    char tmpl[64];
    char *root;
    snprintf(tmpl, sizeof tmpl, "/tmp/%sXXXXXX", prefix);
    root = mkdtemp(tmpl);
    if (!root) return -1;
    snprintf(spec->home, sizeof spec->home, "%s", root);
    snprintf(spec->runtime, sizeof spec->runtime, "%s/run", root);
    snprintf(spec->tools, sizeof spec->tools, "%s/bin", root);
    snprintf(spec->config, sizeof spec->config, "%s/config.json", root);
    snprintf(spec->calls, sizeof spec->calls, "%s/calls.jsonl", root);
    snprintf(spec->profile, sizeof spec->profile, "%s", profile);
    spec->fixture_pid = (int)getpid();
    if (mkdir(spec->runtime, 0700) != 0 || mkdir(spec->tools, 0700) != 0) return -1;
    chmod(spec->runtime, 0700);
    chmod(spec->tools, 0700);
    if (write_text(spec->config, config_json) != 0 || install_tools(spec->tools) != 0) return -1;
    return 0;
}

typedef struct {
    proc *child;
    const char *sock;
} startup;

static int daemon_ready(void *user)
{
    startup *s = user;
    yyjson_doc *doc;
    char err[128];
    int ok;
    if (s->child->done) return 0;
    doc = ipc_request(s->sock, "{\"action\":\"status\"}", 500, err, sizeof err);
    ok = doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok"));
    yyjson_doc_free(doc);
    return ok;
}

static int launch_daemon(proc *child, const char *binary, env_spec *spec, const char *sock)
{
    char *argv[] = {(char *)binary, "serve", NULL};
    char **envp = build_env(spec, NULL, NULL);
    startup probe = {.child = child, .sock = sock};
    int rc;
    if (!envp || proc_start(child, binary, argv, envp) != 0) {
        free_env(envp);
        return -1;
    }
    free_env(envp);
    rc = 0;
    {
        int64_t end = now_ms() + 12000;
        while (now_ms() < end && !daemon_ready(&probe)) usleep(40000);
        if (!daemon_ready(&probe)) rc = -1;
    }
    return rc;
}

static void stop_daemon(proc *child, const char *sock)
{
    char *err;
    if (!child || child->pid <= 0) return;
    kill(child->pid, SIGTERM);
    proc_finish(child, 8000);
    err = proc_stderr(child);
    CHECK(child->signal != SIGKILL);
    CHECK(child->code == 0);
    if (child->code != 0) fprintf(stderr, "%s\n", err);
    free(err);
    CHECK(access(sock, F_OK) != 0);
}

struct burst {
    const char *sock;
    int ok;
};

static void *status_burst(void *arg)
{
    struct burst *b = arg;
    int i;
    b->ok = 1;
    for (i = 0; i < 3; i++) {
        char err[128];
        yyjson_doc *doc = ipc_request(b->sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
        if (!doc || !yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok"))) b->ok = 0;
        yyjson_doc_free(doc);
    }
    return NULL;
}

static int bool_field(yyjson_val *root, const char *key)
{
    return yyjson_is_bool(yyjson_obj_get(root, key));
}

static int daemon_smoke(const char *binary)
{
    env_spec spec;
    char config[1024];
    char voice[PATH_MAX], sock[PATH_MAX], selection[PATH_MAX];
    proc child = {0}, second = {0};
    yyjson_doc *doc;
    char err[256];
    const char *bad[] = {"record", "send", "read", "unknown-command", "fixture-state"};
    const char *flags[] = {"draft", "pending", "retained", "retry", "recording", "transcribing", "speaking"};
    size_t i;
    int idle = -1;
    memset(&spec, 0, sizeof spec);
    snprintf(config, sizeof config,
             "{\"backends\":[{\"id\":\"fixture\",\"listen_url\":\"http://127.0.0.1:9/v1/listen\"}],"
             "\"auto_speak\":false,\"voice_preferences_path\":\"%s/voice-mode\"}",
             spec.home);
    if (prepare(&spec, "voice-daemon-", "{\"backends\":[],\"auto_speak\":false}", "smoke") != 0) return 1;
    snprintf(config, sizeof config,
             "{\"backends\":[{\"id\":\"fixture\",\"listen_url\":\"http://127.0.0.1:9/v1/listen\"}],"
             "\"auto_speak\":false,\"voice_preferences_path\":\"%s/voice-mode\"}", spec.home);
    write_text(spec.config, config);
    CHECK(join_path(voice, sizeof voice, spec.runtime, "pi-voice") == 0);
    CHECK(join_path(sock, sizeof sock, voice, "control.sock") == 0);
    {
        char *argv[] = {(char *)binary, "--help", NULL};
        char **envp = build_env(&spec, NULL, NULL);
        proc help = {0};
        CHECK(proc_start(&help, binary, argv, envp) == 0);
        free_env(envp);
        proc_finish(&help, 5000);
        CHECK(help.code == 0);
        CHECK(help.out && strstr(help.out, "pi-voice"));
        free(help.out); free(help.err);
    }
    CHECK(launch_daemon(&child, binary, &spec, sock) == 0);
    CHECK(mode_is(voice, 0700));
    CHECK(mode_is(sock, 0600));
    doc = ipc_request(sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
    CHECK(doc && !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "phase")), "idle"));
    CHECK(doc && !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "connection_state")), "unselected"));
    CHECK(doc && yyjson_is_arr(yyjson_obj_get(yyjson_doc_get_root(doc), "sessions"))
          && yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "sessions")) == 0);
    CHECK(doc && yyjson_is_false(yyjson_obj_get(yyjson_doc_get_root(doc), "speech_available")));
    for (i = 0; doc && i < sizeof flags / sizeof flags[0]; i++)
        CHECK(bool_field(yyjson_doc_get_root(doc), flags[i]));
    yyjson_doc_free(doc);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char req[128];
        snprintf(req, sizeof req, "{\"action\":\"%s\"}", bad[i]);
        doc = ipc_request(sock, req, 3000, err, sizeof err);
        CHECK(doc && yyjson_is_false(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")));
        yyjson_doc_free(doc);
    }
    doc = ipc_request(sock, "{\"action\":\"team-toggle\"}", 3000, err, sizeof err);
    CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "show_team")));
    yyjson_doc_free(doc);
    CHECK(join_path(selection, sizeof selection, voice, "selection.json") == 0);
    CHECK(mode_is(selection, 0600));
    {
        char *argv[] = {(char *)binary, "serve", NULL};
        char **envp = build_env(&spec, NULL, NULL);
        CHECK(proc_start(&second, binary, argv, envp) == 0);
        free_env(envp);
        proc_finish(&second, 5000);
        CHECK(second.code != 0);
        free(second.out); free(second.err);
        second.pid = -1;
    }
    doc = ipc_request(sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
    CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")));
    yyjson_doc_free(doc);
    {
        struct sockaddr_un addr;
        pthread_t threads[8];
        struct burst bursts[8];
        idle = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, sock, strlen(sock) + 1);
        CHECK(connect(idle, (struct sockaddr *)&addr, sizeof addr) == 0);
        CHECK(write_all(idle, "{\"action\":", 10) == 0);
        for (i = 0; i < 8; i++) {
            bursts[i].sock = sock;
            CHECK(pthread_create(&threads[i], NULL, status_burst, &bursts[i]) == 0);
        }
        for (i = 0; i < 8; i++) {
            pthread_join(threads[i], NULL);
            CHECK(bursts[i].ok);
        }
        stop_daemon(&child, sock);
        close(idle);
        idle = -1;
    }
    CHECK(launch_daemon(&child, binary, &spec, sock) == 0);
    doc = ipc_request(sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
    CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "show_team")));
    yyjson_doc_free(doc);
    for (i = 0; i < 32; i++) {
        struct sockaddr_un addr;
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, sock, strlen(sock) + 1);
        CHECK(connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0);
        CHECK(write_all(fd, "{\"action\":\"status\"}\n", 19) == 0);
        close(fd);
    }
    {
        int64_t end = now_ms() + 3000;
        int ready = 0;
        while (now_ms() < end) {
            CHECK(!child.done);
            doc = ipc_request(sock, "{\"action\":\"status\"}", 500, err, sizeof err);
            ready = doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok"));
            yyjson_doc_free(doc);
            if (ready) break;
            usleep(40000);
        }
        CHECK(ready);
    }
    stop_daemon(&child, sock);
    {
        FILE *fp = fopen(spec.calls, "r");
        char line[1024];
        CHECK(fp != NULL);
        if (fp) {
            while (fgets(line, sizeof line, fp)) {
                yyjson_doc *row = yyjson_read(line, strlen(line), 0);
                yyjson_val *first = row ? yyjson_arr_get_first(yyjson_doc_get_root(row)) : NULL;
                const char *path = first ? yyjson_get_str(first) : "";
                const char *base = strrchr(path ? path : "", '/');
                base = base ? base + 1 : path;
                CHECK(base && !strcmp(base, "pw-dump"));
                yyjson_doc_free(row);
            }
            fclose(fp);
        }
    }
    {
        received box = {0};
        json_srv server = {.fd = -1};
        char ext[PATH_MAX];
        pthread_mutex_init(&box.mu, NULL);
        CHECK(json_listen(&server, sock, smoke_handler, &box) == 0);
        {
            char *argv[] = {(char *)binary, "interact", NULL};
            char **envp = build_env(&spec, NULL, NULL);
            proc uncertain = {0};
            CHECK(proc_start(&uncertain, binary, argv, envp) == 0);
            free_env(envp);
            proc_finish(&uncertain, 5000);
            CHECK(uncertain.code != 0);
            CHECK(uncertain.err && strstr(uncertain.err, "unknown"));
            free(uncertain.out); free(uncertain.err);
        }
        CHECK(box.count == 1 && !strcmp(box.actions[0], "interact"));
        box.count = 0;
        snprintf(ext, sizeof ext, "%s/.pi/agent/extensions", spec.home);
        {
            char marker[PATH_MAX];
            mkdir(spec.home, 0700);
            CHECK(join_path(marker, sizeof marker, spec.home, ".pi") == 0);
            mkdir(marker, 0700);
            CHECK(join_path(marker, sizeof marker, spec.home, ".pi/agent") == 0);
            mkdir(marker, 0700);
            mkdir(ext, 0700);
            CHECK(join_path(marker, sizeof marker, ext, "pi-voice.ts") == 0);
            write_text(marker, "// Disposable installation marker.\n");
        }
        snprintf(spec.pi_result, sizeof spec.pi_result, "%s/pi-launch.json", spec.home);
        {
            char herdr[PATH_MAX];
            char *argv[] = {(char *)binary, "--", "--native-smoke", NULL};
            char **envp;
            proc launched = {0};
            CHECK(join_path(herdr, sizeof herdr, spec.home, "fake-herdr.sock") == 0);
            envp = build_env(&spec, "HERDR_ENV", "1");
            /* build_env only adds one extra pair. Add the rest by extending the array. */
            {
                char pane[64], socket_env[PATH_MAX + 32];
                char **full = calloc(16, sizeof *full);
                int n = 0;
                while (envp[n]) { full[n] = envp[n]; n++; }
                free(envp);
                snprintf(pane, sizeof pane, "HERDR_PANE_ID=disposable-pane");
                snprintf(socket_env, sizeof socket_env, "HERDR_SOCKET_PATH=%s", herdr);
                full[n++] = xdup(pane);
                full[n++] = xdup(socket_env);
                envp = full;
            }
            CHECK(proc_start(&launched, binary, argv, envp) == 0);
            free_env(envp);
            proc_finish(&launched, 10000);
            CHECK(launched.code == 0);
            if (launched.code != 0 && launched.err) fprintf(stderr, "%s\n", launched.err);
            free(launched.out); free(launched.err);
        }
        CHECK(box.count == 3);
        CHECK(!strcmp(box.actions[0], "status"));
        CHECK(!strcmp(box.actions[1], "register"));
        CHECK(!strcmp(box.actions[2], "unregister"));
        {
            FILE *fp = fopen(spec.pi_result, "r");
            char text[1024];
            yyjson_doc *info = NULL;
            size_t n = fp ? fread(text, 1, sizeof text - 1, fp) : 0;
            if (fp) fclose(fp);
            text[n] = '\0';
            info = yyjson_read(text, n, 0);
            CHECK(info);
            if (info) {
                yyjson_val *root = yyjson_doc_get_root(info);
                yyjson_val *args = yyjson_obj_get(root, "args");
                const char *kind = yyjson_get_str(yyjson_obj_get(root, "kind"));
                const char *token = yyjson_get_str(yyjson_obj_get(root, "token"));
                char token_dir[PATH_MAX];
                CHECK(args && yyjson_arr_size(args) == 1 && !strcmp(yyjson_get_str(yyjson_arr_get_first(args)), "--native-smoke"));
                CHECK(kind && !strcmp(kind, "pi"));
                CHECK(token && !strcmp(token, box.tokens[1]) && !strcmp(token, box.tokens[2]));
                CHECK(join_path(token_dir, sizeof token_dir, voice, token ? token : "") == 0);
                CHECK(access(token_dir, F_OK) != 0);
            }
            yyjson_doc_free(info);
        }
        json_close(&server);
        pthread_mutex_destroy(&box.mu);
    }
    if (child.pid > 0) proc_cleanup(&child);
    remove_tree(spec.home);
    if (!failures) puts("Daemon: private IPC, persistence, concurrency, shutdown, launcher and no mutation replay");
    return failures ? 1 : 0;
}

static int staged_ready(void *user)
{
    adapter_state *st = user;
    int ready;
    pthread_mutex_lock(&st->mu);
    ready = st->staged_n >= 4;
    pthread_mutex_unlock(&st->mu);
    return ready;
}

static int native_journey(const char *binary)
{
    env_spec spec;
    http_srv speech = {.fd = -1};
    json_srv adapter = {.fd = -1}, herdr = {.fd = -1};
    adapter_state state;
    proc child = {0};
    int port = 0;
    char config[1200], voice[PATH_MAX], sock[PATH_MAX], adapter_path[PATH_MAX], herdr_path[PATH_MAX];
    char bridge[40], err[256];
    int i;
    memset(&spec, 0, sizeof spec);
    memset(&state, 0, sizeof state);
    if (prepare(&spec, "vj-", "{}", "journey") != 0) return 1;
    if (http_listen(&speech, &port) != 0) return 1;
    snprintf(config, sizeof config,
             "{\"backends\":[{\"id\":\"fixture\",\"listen_url\":\"http://127.0.0.1:%d/v1/listen\"}],"
             "\"auto_speak\":false,\"voice_preferences_path\":\"%s/voice-mode\"}", port, spec.home);
    write_text(spec.config, config);
    CHECK(join_path(voice, sizeof voice, spec.runtime, "pi-voice") == 0);
    mkdir(voice, 0700);
    chmod(voice, 0700);
    snprintf(bridge, sizeof bridge, "aaaaaaaa-bbbb-cccc-dddd-%d", spec.fixture_pid);
    /* canonical 36-char uuid, independent of pid width */
    {
        unsigned char bytes[16];
        FILE *entropy = fopen("/dev/urandom", "rb");
        if (!entropy || fread(bytes, 1, 16, entropy) != 16) {
            for (i = 0; i < 16; i++) bytes[i] = (unsigned char)(i * 17 + 3);
        }
        if (entropy) fclose(entropy);
        snprintf(bridge, sizeof bridge,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                 bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
    {
        char leaf[80];
        snprintf(leaf, sizeof leaf, "pi-%d-%s.sock", spec.fixture_pid, bridge);
        CHECK(join_path(adapter_path, sizeof adapter_path, voice, leaf) == 0);
    }
    CHECK(join_path(herdr_path, sizeof herdr_path, spec.home, "herdr.sock") == 0);
    CHECK(join_path(sock, sizeof sock, voice, "control.sock") == 0);
    pthread_mutex_init(&state.mu, NULL);
    state.pid = spec.fixture_pid;
    snprintf(state.session, sizeof state.session, "native-fixture-session");
    snprintf(state.bridge, sizeof state.bridge, "%s", bridge);
    state.activation = 1;
    snprintf(state.harness, sizeof state.harness, "pi");
    CHECK(json_listen(&adapter, adapter_path, adapter_handler, &state) == 0);
    CHECK(json_listen(&herdr, herdr_path, ok_handler, NULL) == 0);
    if (launch_daemon(&child, binary, &spec, sock) != 0) {
        char *text = proc_stderr(&child);
        fprintf(stderr, "controller startup failed\n%s\n", text);
        free(text);
        failures++;
        goto done;
    }
    {
        yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(mut);
        yyjson_mut_val *target = yyjson_mut_obj(mut);
        char *req;
        yyjson_doc *doc;
        yyjson_mut_doc_set_root(mut, root);
        yyjson_mut_obj_add_strcpy(mut, root, "action", "attach");
        yyjson_mut_obj_add_strcpy(mut, target, "pane", "w1:p1");
        yyjson_mut_obj_add_strcpy(mut, target, "socket", herdr_path);
        yyjson_mut_obj_add_int(mut, target, "pid", spec.fixture_pid);
        yyjson_mut_obj_add_strcpy(mut, target, "session", "native-fixture-session");
        yyjson_mut_obj_add_strcpy(mut, target, "bridge_id", bridge);
        yyjson_mut_obj_add_int(mut, target, "activation", 1);
        yyjson_mut_obj_add_strcpy(mut, target, "adapter_socket", adapter_path);
        yyjson_mut_obj_add_strcpy(mut, target, "harness", "pi");
        yyjson_mut_obj_add_bool(mut, target, "team_child", 0);
        yyjson_mut_obj_add_strcpy(mut, target, "model", "synthetic-model");
        yyjson_mut_obj_add_strcpy(mut, target, "thinking", "medium");
        yyjson_mut_obj_add_val(mut, root, "target", target);
        req = yyjson_mut_write(mut, 0, NULL);
        yyjson_mut_doc_free(mut);
        doc = ipc_request(sock, req ? req : "{}", 3000, err, sizeof err);
        free(req);
        CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")));
        if (doc) {
            const char *token = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "attached"), "token"));
            CHECK(token && token[0]);
            if (token) snprintf(state.token, sizeof state.token, "%s", token);
        }
        yyjson_doc_free(doc);
        mut = yyjson_mut_doc_new(NULL);
        root = yyjson_mut_obj(mut);
        yyjson_mut_doc_set_root(mut, root);
        yyjson_mut_obj_add_strcpy(mut, root, "action", "harness-event");
        yyjson_mut_obj_add_strcpy(mut, root, "token", state.token);
        {
            yyjson_mut_val *event = yyjson_mut_obj(mut);
            yyjson_mut_obj_add_int(mut, event, "pid", spec.fixture_pid);
            yyjson_mut_obj_add_strcpy(mut, event, "session", "native-fixture-session");
            yyjson_mut_obj_add_strcpy(mut, event, "bridge_id", bridge);
            yyjson_mut_obj_add_int(mut, event, "activation", 1);
            yyjson_mut_obj_add_strcpy(mut, event, "harness", "pi");
            yyjson_mut_obj_add_strcpy(mut, event, "type", "ready");
            yyjson_mut_obj_add_bool(mut, event, "ready", 1);
            yyjson_mut_obj_add_bool(mut, event, "accepts_input", 1);
            yyjson_mut_obj_add_strcpy(mut, event, "state", "idle");
            yyjson_mut_obj_add_strcpy(mut, event, "adapter_socket", adapter_path);
            yyjson_mut_obj_add_strcpy(mut, event, "draft_state", "none");
            yyjson_mut_obj_add_val(mut, root, "event", event);
        }
        req = yyjson_mut_write(mut, 0, NULL);
        yyjson_mut_doc_free(mut);
        doc = ipc_request(sock, req ? req : "{}", 3000, err, sizeof err);
        free(req);
        CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "accepted")));
        yyjson_doc_free(doc);
        doc = ipc_request(sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
        CHECK(doc && !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "pane")), "w1:p1"));
        CHECK(doc && !strcmp(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "connection_state")), "ready"));
        yyjson_doc_free(doc);
        mut = yyjson_mut_doc_new(NULL);
        root = yyjson_mut_obj(mut);
        yyjson_mut_doc_set_root(mut, root);
        yyjson_mut_obj_add_strcpy(mut, root, "action", "harness-event");
        yyjson_mut_obj_add_strcpy(mut, root, "token", state.token);
        {
            yyjson_mut_val *event = yyjson_mut_obj(mut);
            yyjson_mut_obj_add_int(mut, event, "pid", spec.fixture_pid);
            yyjson_mut_obj_add_strcpy(mut, event, "session", "foreign");
            yyjson_mut_obj_add_strcpy(mut, event, "bridge_id", bridge);
            yyjson_mut_obj_add_int(mut, event, "activation", 1);
            yyjson_mut_obj_add_strcpy(mut, event, "harness", "pi");
            yyjson_mut_obj_add_strcpy(mut, event, "type", "ready");
            yyjson_mut_obj_add_bool(mut, event, "ready", 1);
            yyjson_mut_obj_add_bool(mut, event, "accepts_input", 1);
            yyjson_mut_obj_add_strcpy(mut, event, "state", "idle");
            yyjson_mut_obj_add_strcpy(mut, event, "adapter_socket", adapter_path);
            yyjson_mut_obj_add_strcpy(mut, event, "draft_state", "none");
            yyjson_mut_obj_add_val(mut, root, "event", event);
        }
        req = yyjson_mut_write(mut, 0, NULL);
        yyjson_mut_doc_free(mut);
        doc = ipc_request(sock, req ? req : "{}", 3000, err, sizeof err);
        free(req);
        CHECK(doc && yyjson_is_false(yyjson_obj_get(yyjson_doc_get_root(doc), "accepted")));
        yyjson_doc_free(doc);
        doc = ipc_request(sock, "{\"action\":\"record\"}", 3000, err, sizeof err);
        CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")));
        yyjson_doc_free(doc);
    }
    {
        int64_t end = now_ms() + 20000;
        while (now_ms() < end && !staged_ready(&state)) usleep(40000);
    }
    CHECK(staged_ready(&state));
    pthread_mutex_lock(&state.mu);
    CHECK(state.submitted_n == 0);
    pthread_mutex_unlock(&state.mu);
    {
        yyjson_doc *doc = ipc_request(sock, "{\"action\":\"interact\"}", 3000, err, sizeof err);
        CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")));
        yyjson_doc_free(doc);
        {
            int64_t end = now_ms() + 12000;
            int draft = 0;
            while (now_ms() < end) {
                yyjson_doc *status = ipc_request(sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
                const char *phase = status ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(status), "phase")) : "";
                draft = phase && !strcmp(phase, "draft");
                yyjson_doc_free(status);
                if (draft) break;
                usleep(40000);
            }
            CHECK(draft);
        }
    }
    for (i = 0; i < 4; i++) {
        char phrase[64];
        snprintf(phrase, sizeof phrase, "Native phrase %d.", i + 1);
        pthread_mutex_lock(&state.mu);
        CHECK(i < state.staged_n && !strcmp(state.staged[i], phrase));
        pthread_mutex_unlock(&state.mu);
    }
    pthread_mutex_lock(&speech.mu);
    CHECK(speech.posts == 4);
    for (i = 0; i < speech.posts && i < 8; i++)
        CHECK(speech.body_n[i] >= 4 && memcmp(speech.bodies[i], "RIFF", 4) == 0);
    pthread_mutex_unlock(&speech.mu);
    {
        yyjson_doc *doc = ipc_request(sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
        yyjson_val *mic = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "microphone") : NULL;
        CHECK(mic && !strcmp(yyjson_get_str(yyjson_obj_get(mic, "name")), "Synthetic microphone"));
        CHECK(mic && yyjson_is_null(yyjson_obj_get(mic, "target")));
        CHECK(mic && yyjson_is_false(yyjson_obj_get(mic, "muted")));
        CHECK(mic && yyjson_is_false(yyjson_obj_get(mic, "missing")));
        yyjson_doc_free(doc);
        doc = ipc_request(sock, "{\"action\":\"send\"}", 3000, err, sizeof err);
        CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok")));
        yyjson_doc_free(doc);
        pthread_mutex_lock(&state.mu);
        CHECK(state.submitted_n == 1);
        CHECK(!strcmp(state.submitted[0], "Native phrase 1.\nNative phrase 2.\nNative phrase 3.\nNative phrase 4."));
        pthread_mutex_unlock(&state.mu);
        doc = ipc_request(sock, "{\"action\":\"send\"}", 3000, err, sizeof err);
        yyjson_doc_free(doc);
        pthread_mutex_lock(&state.mu);
        CHECK(state.submitted_n == 1);
        pthread_mutex_unlock(&state.mu);
        {
            yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *root = yyjson_mut_obj(mut);
            char *req;
            yyjson_mut_doc_set_root(mut, root);
            yyjson_mut_obj_add_strcpy(mut, root, "action", "detach");
            yyjson_mut_obj_add_strcpy(mut, root, "token", state.token);
            yyjson_mut_obj_add_int(mut, root, "pid", spec.fixture_pid);
            yyjson_mut_obj_add_strcpy(mut, root, "session", "native-fixture-session");
            yyjson_mut_obj_add_strcpy(mut, root, "bridge_id", bridge);
            yyjson_mut_obj_add_int(mut, root, "activation", 1);
            yyjson_mut_obj_add_strcpy(mut, root, "harness", "pi");
            req = yyjson_mut_write(mut, 0, NULL);
            yyjson_mut_doc_free(mut);
            doc = ipc_request(sock, req ? req : "{}", 3000, err, sizeof err);
            free(req);
            CHECK(doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "accepted")));
            yyjson_doc_free(doc);
        }
        doc = ipc_request(sock, "{\"action\":\"status\"}", 3000, err, sizeof err);
        CHECK(doc && yyjson_is_arr(yyjson_obj_get(yyjson_doc_get_root(doc), "sessions"))
              && yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "sessions")) == 0);
        yyjson_doc_free(doc);
    }
    stop_daemon(&child, sock);
    if (!failures) puts("Journey: identity guards, four queued PCM slices, STT, staged draft, explicit Send, detach and shutdown");
done:
    proc_cleanup(&child);
    json_close(&adapter);
    json_close(&herdr);
    http_close(&speech);
    pthread_mutex_destroy(&state.mu);
    remove_tree(spec.home);
    return failures ? 1 : 0;
}

static const char *base_name(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

static int is_tool(const char *name)
{
    const char *tools[] = {"systemctl", "pw-dump", "pw-record", "pw-play", "herdr", "wl-copy", "pi"};
    size_t i;
    for (i = 0; i < sizeof tools / sizeof tools[0]; i++) if (!strcmp(name, tools[i])) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    const char *base = "";
    const char *arg0;
    int before;
    signal(SIGPIPE, ignore_pipe);
    if (n > 0) {
        exe[n] = '\0';
        base = base_name(exe);
    }
    arg0 = base_name(argc > 0 ? argv[0] : "");
    if (is_tool(base)) return tool_main(exe, argc, argv);
    if (is_tool(arg0)) return tool_main(argv[0], argc, argv);
    if (argc == 3 && !strcmp(argv[1], "--tool")) return tool_main(argv[2], argc - 2, argv + 2);
    if (argc != 2) {
        fprintf(stderr, "usage: %s PI_VOICE_BINARY\n", argv[0]);
        return 2;
    }
    before = failures;
    if (daemon_smoke(argv[1]) != 0) return 1;
    failures = before;
    if (native_journey(argv[1]) != 0) return 1;
    return 0;
}
