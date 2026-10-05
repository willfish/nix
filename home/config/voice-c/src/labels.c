#define _POSIX_C_SOURCE 200809L

#include "labels.h"

#include <glib.h>
#include <yyjson.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct SnapshotState {
    double fetched_at;
    double attempted_at;
    int outcome;
    int inflight;
    size_t count;
    size_t cap;
    char **ids;
    char **workspace;
    char **tab;
    char **pane;
    char **display_agent;
};

typedef struct {
    SocketKey key;
    uint64_t ticket;
    int live;
    SnapshotState state;
} CacheEntry;

typedef struct Job {
    void (*fn)(void *);
    void (*dtor)(void *);
    void *arg;
    struct Job *next;
} Job;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t threads[LABELS_MAX_WORKERS];
    int nthreads;
    int stop;
    Job *head;
    Job *tail;
} Pool;

struct LabelsCache {
    labels_runner_fn runner;
    void *runner_user;
    labels_clock_fn clock;
    void *clock_user;
    labels_submit_fn submit;
    void *sched;
    int max_keys;
    int owned_pool;
    int closed;
    int pending;
    uint64_t ticket_gen;
    pthread_mutex_t mu;
    Pool *pool;
    CacheEntry *entries;
    size_t count;
};

typedef struct {
    LabelsCache *cache;
    SocketKey key;
    uint64_t ticket;
} RefreshArg;

static labels_stat_fn g_stat_fn;
static void *g_stat_user;
static labels_exec_fn g_exec_fn;
static void *g_exec_user;
static _Thread_local char g_error[256];

static void set_error(const char *msg) {
    snprintf(g_error, sizeof g_error, "%s", msg ? msg : "");
}

const char *labels_last_error(void) {
    return g_error;
}

void labels_set_stat_fn(labels_stat_fn fn, void *user) {
    g_stat_fn = fn;
    g_stat_user = user;
}

void labels_set_exec_fn(labels_exec_fn fn, void *user) {
    g_exec_fn = fn;
    g_exec_user = user;
}

static char *xdup(const char *text) {
    const char *src = text ? text : "";
    size_t n = strlen(src);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n + 1);
    return out;
}

static int grow(char **buf, size_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t next = *cap ? *cap : 64;
    while (next < need) {
        if (next > (SIZE_MAX / 2)) return -1;
        next *= 2;
    }
    char *p = realloc(*buf, next);
    if (!p) return -1;
    *buf = p;
    *cap = next;
    return 0;
}

int socket_key_init(SocketKey *key, const char *path, uint64_t device, uint64_t inode) {
    if (!key) return LABELS_ERR;
    key->path = xdup(path);
    if (!key->path) return LABELS_ERR;
    key->device = device;
    key->inode = inode;
    return LABELS_OK;
}

void socket_key_clear(SocketKey *key) {
    if (!key) return;
    free(key->path);
    key->path = NULL;
    key->device = 0;
    key->inode = 0;
}

int socket_key_equal(const SocketKey *a, const SocketKey *b) {
    if (!a || !b || !a->path || !b->path) return 0;
    return a->device == b->device && a->inode == b->inode && strcmp(a->path, b->path) == 0;
}

static int socket_key_copy(SocketKey *dst, const SocketKey *src) {
    return socket_key_init(dst, src && src->path ? src->path : "", src ? src->device : 0, src ? src->inode : 0);
}

static int codepoint_width(gunichar c) {
    GUnicodeType type = g_unichar_type(c);
    if (type == G_UNICODE_NON_SPACING_MARK || type == G_UNICODE_ENCLOSING_MARK) return 0;
    if (g_unichar_iswide(c)) return 2;
    return 1;
}

static int is_category_c(gunichar c) {
    GUnicodeType type = g_unichar_type(c);
    return type == G_UNICODE_CONTROL || type == G_UNICODE_FORMAT || type == G_UNICODE_SURROGATE
        || type == G_UNICODE_PRIVATE_USE || type == G_UNICODE_UNASSIGNED;
}

int labels_display_width(const char *text) {
    if (!text || !text[0]) return 0;
    int width = 0;
    const char *p = text;
    while (*p) {
        const char *next = g_utf8_next_char(p);
        if (next == p) {
            width += 1;
            p++;
            continue;
        }
        width += codepoint_width(g_utf8_get_char(p));
        p = next;
    }
    return width;
}

static char *labels_clean(const char *text) {
    GString *raw = g_string_new("");
    GString *out = g_string_new("");
    if (!raw || !out) {
        if (raw) g_string_free(raw, TRUE);
        if (out) g_string_free(out, TRUE);
        return NULL;
    }
    if (text) {
        const char *p = text;
        while (*p) {
            const char *next = g_utf8_next_char(p);
            if (next == p) {
                if ((unsigned char)*p >= 32) g_string_append_c(raw, *p);
                else g_string_append_c(raw, ' ');
                p++;
                continue;
            }
            gunichar c = g_utf8_get_char(p);
            if (is_category_c(c)) g_string_append_c(raw, ' ');
            else g_string_append_unichar(raw, c);
            p = next;
        }
    }
    int token = 0;
    const char *p = raw->str;
    while (*p) {
        const char *next = g_utf8_next_char(p);
        gunichar c = next == p ? (unsigned char)*p : g_utf8_get_char(p);
        int space = g_unichar_isspace(c) || c == ' ';
        if (space) {
            token = 0;
        } else {
            if (out->len && !token) g_string_append_c(out, ' ');
            if (next == p) g_string_append_c(out, *p);
            else g_string_append_unichar(out, c);
            token = 1;
        }
        p = next == p ? p + 1 : next;
    }
    g_string_free(raw, TRUE);
    return g_string_free(out, FALSE);
}

static int is_ws(gunichar c) {
    return g_unichar_isspace(c) || c == ' ';
}

static char *truncate_text(const char *text, int budget) {
    if (budget <= 0) return xdup("");
    if (!text) text = "";
    if (labels_display_width(text) <= budget) return xdup(text);
    size_t cap = 32;
    gunichar *cps = malloc(cap * sizeof *cps);
    if (!cps) return NULL;
    size_t n = 0;
    int width = 0;
    const char *p = text;
    while (*p) {
        const char *next = g_utf8_next_char(p);
        gunichar c = next == p ? (unsigned char)*p : g_utf8_get_char(p);
        int cw = codepoint_width(c);
        if (width + cw > budget - 1) break;
        if (n == cap) {
            size_t ncap = cap * 2;
            gunichar *grown = realloc(cps, ncap * sizeof *cps);
            if (!grown) {
                free(cps);
                return NULL;
            }
            cps = grown;
            cap = ncap;
        }
        cps[n++] = c;
        width += cw;
        p = next == p ? p + 1 : next;
    }
    while (n > 0 && is_ws(cps[n - 1])) n--;
    GString *out = g_string_new("");
    for (size_t i = 0; i < n; i++) g_string_append_unichar(out, cps[i]);
    g_string_append_unichar(out, 0x2026);
    free(cps);
    return g_string_free(out, FALSE);
}

static void state_init(SnapshotState *state) {
    memset(state, 0, sizeof *state);
}

static void state_clear(SnapshotState *state) {
    if (!state) return;
    for (size_t i = 0; i < state->count; i++) {
        free(state->ids[i]);
        free(state->workspace[i]);
        free(state->tab[i]);
        free(state->pane[i]);
        free(state->display_agent[i]);
    }
    free(state->ids);
    free(state->workspace);
    free(state->tab);
    free(state->pane);
    free(state->display_agent);
    memset(state, 0, sizeof *state);
}

void labels_state_free(SnapshotState *state) {
    if (!state) return;
    state_clear(state);
    free(state);
}

static SnapshotState *state_new(void) {
    SnapshotState *state = calloc(1, sizeof *state);
    return state;
}

static void *grow_ptr(void *ptr, size_t cap, size_t elem) {
    return realloc(ptr, cap * elem);
}

static int state_add(SnapshotState *state, const char *id, const char *workspace,
    const char *tab, const char *pane, const char *display_agent) {
    if (state->count == state->cap) {
        size_t cap = state->cap ? state->cap * 2 : 4;
        char **ids = grow_ptr(state->ids, cap, sizeof *ids);
        if (!ids) return LABELS_ERR;
        state->ids = ids;
        char **ws = grow_ptr(state->workspace, cap, sizeof *ws);
        if (!ws) return LABELS_ERR;
        state->workspace = ws;
        char **tabs = grow_ptr(state->tab, cap, sizeof *tabs);
        if (!tabs) return LABELS_ERR;
        state->tab = tabs;
        char **panes = grow_ptr(state->pane, cap, sizeof *panes);
        if (!panes) return LABELS_ERR;
        state->pane = panes;
        char **agents = grow_ptr(state->display_agent, cap, sizeof *agents);
        if (!agents) return LABELS_ERR;
        state->display_agent = agents;
        state->cap = cap;
    }
    size_t i = state->count;
    state->ids[i] = xdup(id);
    state->workspace[i] = xdup(workspace);
    state->tab[i] = xdup(tab);
    state->pane[i] = xdup(pane);
    state->display_agent[i] = xdup(display_agent);
    if (!state->ids[i] || !state->workspace[i] || !state->tab[i] || !state->pane[i] || !state->display_agent[i])
        return LABELS_ERR;
    state->count++;
    return LABELS_OK;
}

static SnapshotState *state_copy(const SnapshotState *src) {
    SnapshotState *dst = state_new();
    if (!dst) return NULL;
    if (!src) return dst;
    dst->fetched_at = src->fetched_at;
    dst->attempted_at = src->attempted_at;
    dst->outcome = src->outcome;
    dst->inflight = src->inflight;
    for (size_t i = 0; i < src->count; i++) {
        if (state_add(dst, src->ids[i], src->workspace[i], src->tab[i], src->pane[i], src->display_agent[i]) != LABELS_OK) {
            labels_state_free(dst);
            return NULL;
        }
    }
    return dst;
}

static ssize_t state_find(const SnapshotState *state, const char *pane_id) {
    if (!state || !pane_id) return -1;
    for (size_t i = 0; i < state->count; i++) {
        if (strcmp(state->ids[i], pane_id) == 0) return (ssize_t)i;
    }
    return -1;
}

int labels_state_outcome(const SnapshotState *state) {
    return state ? state->outcome : LABELS_OUTCOME_UNKNOWN;
}

int labels_state_inflight(const SnapshotState *state) {
    return state ? state->inflight : 0;
}

double labels_state_fetched_at(const SnapshotState *state) {
    return state ? state->fetched_at : 0;
}

double labels_state_attempted_at(const SnapshotState *state) {
    return state ? state->attempted_at : 0;
}

int labels_state_authoritative(const SnapshotState *state, double now) {
    if (!state || state->outcome != LABELS_OUTCOME_OK) return 0;
    double age = now - state->fetched_at;
    return age >= 0 && age < LABELS_AUTHORITY_SEC;
}

size_t labels_state_pane_count(const SnapshotState *state) {
    return state ? state->count : 0;
}

const char *labels_state_pane_id(const SnapshotState *state, size_t index) {
    if (!state || index >= state->count) return NULL;
    return state->ids[index];
}

const char *labels_state_field(const SnapshotState *state, const char *pane_id, const char *field) {
    ssize_t i = state_find(state, pane_id);
    if (i < 0 || !field) return NULL;
    if (strcmp(field, "workspace") == 0) return state->workspace[i];
    if (strcmp(field, "tab") == 0) return state->tab[i];
    if (strcmp(field, "pane") == 0) return state->pane[i];
    if (strcmp(field, "display_agent") == 0) return state->display_agent[i];
    return NULL;
}

int labels_state_set_field(SnapshotState *state, const char *pane_id, const char *field, const char *value) {
    (void)state;
    (void)pane_id;
    (void)field;
    (void)value;
    set_error("snapshot panes are immutable");
    return LABELS_ERR;
}

static const char *json_string(yyjson_val *value) {
    if (!value || !yyjson_is_str(value)) return NULL;
    return yyjson_get_str(value);
}

static const char *or_text(yyjson_val *value, int *stop) {
    *stop = 0;
    if (!value || yyjson_is_null(value)) return NULL;
    if (yyjson_is_str(value)) {
        const char *text = yyjson_get_str(value);
        if (text && text[0]) return text;
        return NULL;
    }
    *stop = 1;
    return "";
}

static int index_rows(yyjson_val *rows, const char *identity, yyjson_val ***out, size_t *out_count) {
    if (!yyjson_is_arr(rows)) {
        set_error("missing snapshot list");
        return LABELS_INVALID;
    }
    size_t n = yyjson_arr_size(rows);
    yyjson_val **index = calloc(n ? n : 1, sizeof *index);
    char **ids = calloc(n ? n : 1, sizeof *ids);
    if (!index || !ids) {
        free(index);
        free(ids);
        return LABELS_ERR;
    }
    size_t count = 0;
    size_t idx, max;
    yyjson_val *row;
    yyjson_arr_foreach(rows, idx, max, row) {
        (void)max;
        const char *id = json_string(yyjson_obj_get(row, identity));
        if (!yyjson_is_obj(row) || !id) {
            set_error("invalid snapshot row");
            free(index);
            free(ids);
            return LABELS_INVALID;
        }
        for (size_t i = 0; i < count; i++) {
            if (strcmp(ids[i], id) == 0) {
                set_error("duplicate snapshot identity");
                free(index);
                free(ids);
                return LABELS_INVALID;
            }
        }
        ids[count] = (char *)id;
        index[count] = row;
        count++;
    }
    free(ids);
    *out = index;
    *out_count = count;
    return LABELS_OK;
}

static yyjson_val *find_row(yyjson_val **rows, size_t count, const char *identity, const char *id) {
    if (!id) return NULL;
    for (size_t i = 0; i < count; i++) {
        const char *row_id = json_string(yyjson_obj_get(rows[i], identity));
        if (row_id && strcmp(row_id, id) == 0) return rows[i];
    }
    return NULL;
}

SnapshotState *labels_parse_snapshot(const char *json) {
    set_error("");
    if (!json) {
        set_error("snapshot must be an object");
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        set_error("snapshot must be an object");
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        set_error("snapshot must be an object");
        yyjson_doc_free(doc);
        return NULL;
    }
    yyjson_val **workspaces = NULL, **tabs = NULL, **panes = NULL;
    size_t nw = 0, nt = 0, np = 0;
    int rc = index_rows(yyjson_obj_get(root, "workspaces"), "workspace_id", &workspaces, &nw);
    if (rc == LABELS_OK) rc = index_rows(yyjson_obj_get(root, "tabs"), "tab_id", &tabs, &nt);
    if (rc == LABELS_OK) rc = index_rows(yyjson_obj_get(root, "panes"), "pane_id", &panes, &np);
    SnapshotState *state = rc == LABELS_OK ? state_new() : NULL;
    if (rc == LABELS_OK && !state) rc = LABELS_ERR;
    if (rc == LABELS_OK) {
        for (size_t i = 0; i < np; i++) {
            const char *id = json_string(yyjson_obj_get(panes[i], "pane_id"));
            const char *ws_id = json_string(yyjson_obj_get(panes[i], "workspace_id"));
            const char *tab_id = json_string(yyjson_obj_get(panes[i], "tab_id"));
            yyjson_val *ws = find_row(workspaces, nw, "workspace_id", ws_id);
            yyjson_val *tab = find_row(tabs, nt, "tab_id", tab_id);
            char *workspace = labels_clean(ws && yyjson_is_obj(ws) ? json_string(yyjson_obj_get(ws, "label")) : NULL);
            char *tab_label = labels_clean(tab && yyjson_is_obj(tab) ? json_string(yyjson_obj_get(tab, "label")) : NULL);
            int stop = 0;
            const char *pane_src = or_text(yyjson_obj_get(panes[i], "label"), &stop);
            if (!pane_src && !stop) pane_src = or_text(yyjson_obj_get(panes[i], "title"), &stop);
            if (!pane_src) pane_src = "";
            char *pane = labels_clean(pane_src);
            char *agent = labels_clean(json_string(yyjson_obj_get(panes[i], "display_agent")));
            if (!workspace || !tab_label || !pane || !agent
                || state_add(state, id, workspace, tab_label, pane, agent) != LABELS_OK) {
                rc = LABELS_ERR;
            }
            free(workspace);
            free(tab_label);
            free(pane);
            free(agent);
            if (rc != LABELS_OK) break;
        }
    }
    free(workspaces);
    free(tabs);
    free(panes);
    yyjson_doc_free(doc);
    if (rc != LABELS_OK) {
        labels_state_free(state);
        if (!g_error[0]) set_error("invalid snapshot");
        return NULL;
    }
    return state;
}

static int default_stat(const char *path, uint64_t *device, uint64_t *inode, void *user) {
    (void)user;
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    *device = (uint64_t)st.st_dev;
    *inode = (uint64_t)st.st_ino;
    return 0;
}

static char *find_executable(const char *file) {
    if (!file || !file[0]) return NULL;
    if (strchr(file, '/')) return access(file, X_OK) == 0 ? xdup(file) : NULL;
    const char *path = getenv("PATH");
    if (!path) path = "/run/current-system/sw/bin:/usr/bin:/bin";
    char *copy = xdup(path);
    if (!copy) return NULL;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        size_t n = strlen(dir) + 1 + strlen(file) + 1;
        char *candidate = malloc(n);
        if (!candidate) break;
        snprintf(candidate, n, "%s/%s", dir, file);
        if (access(candidate, X_OK) == 0) {
            free(copy);
            return candidate;
        }
        free(candidate);
    }
    free(copy);
    return NULL;
}

static char **build_env(const char *socket_path) {
    extern char **environ;
    int count = 0;
    while (environ && environ[count]) count++;
    char **envp = calloc((size_t)count + 2, sizeof *envp);
    if (!envp) return NULL;
    char *assignment = NULL;
    size_t alen = strlen("HERDR_SOCKET_PATH=") + strlen(socket_path) + 1;
    assignment = malloc(alen);
    if (!assignment) {
        free(envp);
        return NULL;
    }
    snprintf(assignment, alen, "HERDR_SOCKET_PATH=%s", socket_path);
    int replaced = 0;
    for (int i = 0; i < count; i++) {
        if (strncmp(environ[i], "HERDR_SOCKET_PATH=", 18) == 0) {
            envp[i] = assignment;
            replaced = 1;
        } else {
            envp[i] = environ[i];
        }
    }
    if (!replaced) envp[count++] = assignment;
    envp[count] = NULL;
    return envp;
}

static void free_env(char **envp) {
    if (!envp) return;
    for (int i = 0; envp[i]; i++) {
        if (strncmp(envp[i], "HERDR_SOCKET_PATH=", 18) == 0) free(envp[i]);
    }
    free(envp);
}

static int append_bytes(char **buf, size_t *len, size_t *cap, const char *data, size_t n) {
    if (grow(buf, cap, *len + n + 1) != 0) return -1;
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

static int run_process(const char *socket_path, double timeout, char **stdout_text) {
    *stdout_text = NULL;
    if (timeout < 0) {
        set_error("invalid timeout");
        return LABELS_ERR;
    }
    char *binary = find_executable("herdr");
    if (!binary) {
        set_error("herdr not found");
        return LABELS_ERR;
    }
    char **envp = build_env(socket_path ? socket_path : "");
    if (!envp) {
        free(binary);
        return LABELS_ERR;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        free_env(envp);
        free(binary);
        set_error("pipe failed");
        return LABELS_ERR;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        free_env(envp);
        free(binary);
        set_error("fork failed");
        return LABELS_ERR;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (fds[1] != STDOUT_FILENO) close(fds[1]);
        char *argv[] = {"herdr", "api", "snapshot", NULL};
        execve(binary, argv, envp);
        _exit(127);
    }
    close(fds[1]);
    free(binary);
    free_env(envp);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int timed_out = 0;
    int status = 0;
    int exited = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        int remain = (int)((timeout - elapsed) * 1000.0);
        if (remain < 0) remain = 0;
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int pr = poll(&pfd, 1, remain);
        if (pr > 0) {
            char tmp[4096];
            ssize_t n = read(fds[0], tmp, sizeof tmp);
            if (n > 0) {
                if (append_bytes(&buf, &len, &cap, tmp, (size_t)n) != 0) {
                    timed_out = 1;
                    break;
                }
            } else if (n == 0) {
                break;
            }
        }
        pid_t wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid) {
            exited = 1;
            char tmp[4096];
            ssize_t n;
            while ((n = read(fds[0], tmp, sizeof tmp)) > 0) {
                if (append_bytes(&buf, &len, &cap, tmp, (size_t)n) != 0) break;
            }
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= timeout) {
            timed_out = 1;
            break;
        }
    }
    close(fds[0]);
    if (timed_out || !exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(buf);
        set_error("herdr snapshot timed out");
        return LABELS_ERR;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(buf);
        set_error("herdr snapshot failed");
        return LABELS_ERR;
    }
    if (!buf) buf = xdup("");
    *stdout_text = buf;
    return LABELS_OK;
}

int labels_run_snapshot(const SocketKey *key, double timeout, char **snapshot_json) {
    if (snapshot_json) *snapshot_json = NULL;
    set_error("");
    if (!key || !key->path || !snapshot_json) {
        set_error("invalid socket key");
        return LABELS_ERR;
    }
    labels_stat_fn stat_fn = g_stat_fn ? g_stat_fn : default_stat;
    uint64_t device = 0, inode = 0;
    if (stat_fn(key->path, &device, &inode, g_stat_user) != 0 || device != key->device || inode != key->inode) {
        set_error("Herdr socket instance changed");
        return LABELS_ERR;
    }
    char *stdout_text = NULL;
    int rc;
    if (g_exec_fn) rc = g_exec_fn(key->path, timeout, &stdout_text, g_exec_user);
    else rc = run_process(key->path, timeout, &stdout_text);
    if (rc != 0) {
        free(stdout_text);
        if (!g_error[0]) set_error("herdr snapshot failed");
        return LABELS_ERR;
    }
    if (stat_fn(key->path, &device, &inode, g_stat_user) != 0 || device != key->device || inode != key->inode) {
        free(stdout_text);
        set_error("Herdr socket instance changed");
        return LABELS_ERR;
    }
    yyjson_doc *doc = stdout_text ? yyjson_read(stdout_text, strlen(stdout_text), 0) : NULL;
    free(stdout_text);
    if (!doc) {
        set_error("invalid herdr response");
        return LABELS_ERR;
    }
    yyjson_val *snapshot = yyjson_ptr_get(yyjson_doc_get_root(doc), "/result/snapshot");
    if (!snapshot) {
        yyjson_doc_free(doc);
        set_error("invalid herdr response");
        return LABELS_ERR;
    }
    *snapshot_json = yyjson_val_write(snapshot, 0, NULL);
    yyjson_doc_free(doc);
    if (!*snapshot_json) return LABELS_ERR;
    return LABELS_OK;
}

static void *pool_worker(void *arg) {
    Pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mu);
        while (!pool->stop && !pool->head) pthread_cond_wait(&pool->cv, &pool->mu);
        if (!pool->head) {
            pthread_mutex_unlock(&pool->mu);
            return NULL;
        }
        Job *job = pool->head;
        pool->head = job->next;
        if (!pool->head) pool->tail = NULL;
        pthread_mutex_unlock(&pool->mu);
        job->fn(job->arg);
        free(job);
    }
}

static Pool *pool_new(int workers) {
    if (workers < 1) workers = 1;
    if (workers > LABELS_MAX_WORKERS) workers = LABELS_MAX_WORKERS;
    Pool *pool = calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->cv, NULL);
    pool->nthreads = workers;
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, pool_worker, pool) != 0) {
            pthread_mutex_lock(&pool->mu);
            pool->stop = 1;
            pthread_cond_broadcast(&pool->cv);
            pthread_mutex_unlock(&pool->mu);
            for (int j = 0; j < i; j++) pthread_join(pool->threads[j], NULL);
            pthread_mutex_destroy(&pool->mu);
            pthread_cond_destroy(&pool->cv);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

static int pool_submit(Pool *pool, void (*fn)(void *), void (*dtor)(void *), void *arg) {
    Job *job = calloc(1, sizeof *job);
    if (!job) return LABELS_ERR;
    job->fn = fn;
    job->dtor = dtor;
    job->arg = arg;
    pthread_mutex_lock(&pool->mu);
    if (pool->stop) {
        pthread_mutex_unlock(&pool->mu);
        free(job);
        return LABELS_ERR;
    }
    if (pool->tail) pool->tail->next = job;
    else pool->head = job;
    pool->tail = job;
    pthread_cond_signal(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
    return LABELS_OK;
}

static void pool_shutdown(Pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->mu);
    pool->stop = 1;
    Job *job = pool->head;
    pool->head = NULL;
    pool->tail = NULL;
    pthread_cond_broadcast(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
    while (job) {
        Job *next = job->next;
        if (job->dtor) job->dtor(job->arg);
        else free(job->arg);
        free(job);
        job = next;
    }
    for (int i = 0; i < pool->nthreads; i++) pthread_join(pool->threads[i], NULL);
    pthread_mutex_destroy(&pool->mu);
    pthread_cond_destroy(&pool->cv);
    free(pool);
}

static double default_clock(void *user) {
    (void)user;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int default_runner(const SocketKey *key, double timeout, char **snapshot_json, void *user) {
    (void)user;
    return labels_run_snapshot(key, timeout, snapshot_json);
}

static void entry_clear(CacheEntry *entry) {
    socket_key_clear(&entry->key);
    state_clear(&entry->state);
    entry->live = 0;
}

static CacheEntry *find_entry(LabelsCache *cache, const SocketKey *key) {
    for (size_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].live && socket_key_equal(&cache->entries[i].key, key)) return &cache->entries[i];
    }
    return NULL;
}

static void publish(LabelsCache *cache, const SocketKey *key, uint64_t ticket, SnapshotState *panes) {
    pthread_mutex_lock(&cache->mu);
    if (cache->pending > 0) cache->pending--;
    CacheEntry *entry = find_entry(cache, key);
    if (cache->closed || !entry || entry->ticket != ticket) {
        pthread_mutex_unlock(&cache->mu);
        return;
    }
    double now = cache->clock ? cache->clock(cache->clock_user) : default_clock(NULL);
    if (!panes) {
        entry->state.attempted_at = now;
        entry->state.outcome = LABELS_OUTCOME_UNAVAILABLE;
        entry->state.inflight = 0;
    } else {
        SnapshotState saved = entry->state;
        entry->state = *panes;
        memset(panes, 0, sizeof *panes);
        entry->state.fetched_at = now;
        entry->state.attempted_at = now;
        entry->state.outcome = LABELS_OUTCOME_OK;
        entry->state.inflight = 0;
        state_clear(&saved);
    }
    pthread_mutex_unlock(&cache->mu);
}

static void refresh_job(void *arg) {
    RefreshArg *job = arg;
    char *json = NULL;
    SnapshotState *panes = NULL;
    if (job->cache->runner && job->cache->runner(&job->key, LABELS_SNAPSHOT_TIMEOUT, &json, job->cache->runner_user) == 0)
        panes = labels_parse_snapshot(json);
    free(json);
    publish(job->cache, &job->key, job->ticket, panes);
    labels_state_free(panes);
    socket_key_clear(&job->key);
    free(job);
}

static void refresh_cancel(void *arg) {
    RefreshArg *job = arg;
    pthread_mutex_lock(&job->cache->mu);
    if (job->cache->pending > 0) job->cache->pending--;
    pthread_mutex_unlock(&job->cache->mu);
    socket_key_clear(&job->key);
    free(job);
}

LabelsCache *labels_cache_new(const LabelsCacheConfig *cfg) {
    int max_keys = cfg && cfg->max_keys ? cfg->max_keys : LABELS_MAX_KEYS;
    if (max_keys < 1) {
        set_error("max_keys must be positive");
        return NULL;
    }
    LabelsCache *cache = calloc(1, sizeof *cache);
    if (!cache) return NULL;
    cache->max_keys = max_keys;
    cache->runner = cfg && cfg->runner ? cfg->runner : default_runner;
    cache->runner_user = cfg ? cfg->runner_user : NULL;
    cache->clock = cfg && cfg->clock ? cfg->clock : default_clock;
    cache->clock_user = cfg ? cfg->clock_user : NULL;
    cache->submit = cfg ? cfg->submit : NULL;
    cache->sched = cfg ? cfg->sched : NULL;
    pthread_mutex_init(&cache->mu, NULL);
    if (!cache->submit) {
        int workers = max_keys < LABELS_MAX_WORKERS ? max_keys : LABELS_MAX_WORKERS;
        cache->pool = pool_new(workers);
        if (!cache->pool) {
            pthread_mutex_destroy(&cache->mu);
            free(cache);
            return NULL;
        }
        cache->owned_pool = 1;
    }
    return cache;
}

void labels_cache_close(LabelsCache *cache) {
    if (!cache) return;
    pthread_mutex_lock(&cache->mu);
    cache->closed = 1;
    for (size_t i = 0; i < cache->count; i++) entry_clear(&cache->entries[i]);
    free(cache->entries);
    cache->entries = NULL;
    cache->count = 0;
    pthread_mutex_unlock(&cache->mu);
    if (cache->owned_pool && cache->pool) {
        pool_shutdown(cache->pool);
        cache->pool = NULL;
        cache->owned_pool = 0;
    }
}

void labels_cache_free(LabelsCache *cache) {
    if (!cache) return;
    labels_cache_close(cache);
    pthread_mutex_destroy(&cache->mu);
    free(cache);
}

int labels_cache_set_active(LabelsCache *cache, const SocketKey *keys, size_t count) {
    if (!cache) return LABELS_ERR;
    size_t unique = 0;
    for (size_t i = 0; i < count; i++) {
        int seen = 0;
        for (size_t j = 0; j < i; j++) {
            if (socket_key_equal(&keys[i], &keys[j])) seen = 1;
        }
        if (!seen) unique++;
    }
    if ((int)unique > cache->max_keys) {
        set_error("too many active Herdr instances");
        return LABELS_TOO_MANY;
    }
    pthread_mutex_lock(&cache->mu);
    if (cache->closed) {
        pthread_mutex_unlock(&cache->mu);
        set_error("snapshot cache closed");
        return LABELS_CLOSED;
    }
    CacheEntry *next = unique ? calloc(unique, sizeof *next) : NULL;
    if (unique && !next) {
        pthread_mutex_unlock(&cache->mu);
        return LABELS_ERR;
    }
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        int seen = 0;
        for (size_t j = 0; j < i; j++) {
            if (socket_key_equal(&keys[i], &keys[j])) seen = 1;
        }
        if (seen) continue;
        CacheEntry *old = find_entry(cache, &keys[i]);
        if (old) {
            next[n] = *old;
            old->live = 0;
        } else {
            if (socket_key_copy(&next[n].key, &keys[i]) != LABELS_OK) {
                for (size_t k = 0; k < n; k++) entry_clear(&next[k]);
                free(next);
                pthread_mutex_unlock(&cache->mu);
                return LABELS_ERR;
            }
            next[n].ticket = ++cache->ticket_gen;
            state_init(&next[n].state);
        }
        next[n].live = 1;
        n++;
    }
    for (size_t i = 0; i < cache->count; i++) {
        if (cache->entries[i].live) entry_clear(&cache->entries[i]);
    }
    free(cache->entries);
    cache->entries = next;
    cache->count = n;
    pthread_mutex_unlock(&cache->mu);
    return LABELS_OK;
}

SnapshotState *labels_cache_read(LabelsCache *cache, const SocketKey *key) {
    if (!cache) return state_new();
    pthread_mutex_lock(&cache->mu);
    CacheEntry *entry = find_entry(cache, key);
    SnapshotState *copy = state_copy(entry ? &entry->state : NULL);
    pthread_mutex_unlock(&cache->mu);
    return copy;
}

int labels_cache_refresh(LabelsCache *cache, const SocketKey *key) {
    if (!cache || !key) return 0;
    RefreshArg *job = calloc(1, sizeof *job);
    if (!job) return 0;
    if (socket_key_copy(&job->key, key) != LABELS_OK) {
        free(job);
        return 0;
    }
    job->cache = cache;
    pthread_mutex_lock(&cache->mu);
    CacheEntry *entry = find_entry(cache, key);
    double now = cache->clock ? cache->clock(cache->clock_user) : 0;
    if (cache->closed || !entry || entry->state.inflight
        || (entry->state.outcome != LABELS_OUTCOME_UNKNOWN && now - entry->state.attempted_at < LABELS_REFRESH_SEC)
        || cache->pending >= cache->max_keys) {
        pthread_mutex_unlock(&cache->mu);
        socket_key_clear(&job->key);
        free(job);
        return 0;
    }
    cache->pending++;
    entry->state.attempted_at = now;
    entry->state.inflight = 1;
    job->ticket = entry->ticket;
    pthread_mutex_unlock(&cache->mu);
    int rc;
    if (cache->submit) rc = cache->submit(refresh_job, job, cache->sched);
    else rc = pool_submit(cache->pool, refresh_job, refresh_cancel, job);
    if (rc != 0) {
        publish(cache, key, job->ticket, NULL);
        socket_key_clear(&job->key);
        free(job);
    }
    return 1;
}

static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256(const uint8_t *msg, size_t len, uint8_t out[32]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t blocks = (len + 8) / 64 + 1;
    uint8_t *buf = calloc(blocks, 64);
    if (!buf) {
        memset(out, 0, 32);
        return;
    }
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t b = 0; b < blocks; b++) {
        uint32_t w[64];
        const uint8_t *block = buf + b * 64;
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16)
                | ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b0 = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b0) ^ (a & c) ^ (b0 & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b0;
            b0 = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b0;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    free(buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

static void append_json_string(GString *out, const char *text) {
    g_string_append_c(out, '"');
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            g_string_append_c(out, '\\');
            g_string_append_c(out, (char)c);
            p++;
        } else if (c == '\b') {
            g_string_append(out, "\\b");
            p++;
        } else if (c == '\f') {
            g_string_append(out, "\\f");
            p++;
        } else if (c == '\n') {
            g_string_append(out, "\\n");
            p++;
        } else if (c == '\r') {
            g_string_append(out, "\\r");
            p++;
        } else if (c == '\t') {
            g_string_append(out, "\\t");
            p++;
        } else if (c < 0x20 || c == 0x7f) {
            g_string_append_printf(out, "\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            g_string_append_c(out, (char)c);
            p++;
        } else {
            const char *next = g_utf8_next_char((const char *)p);
            gunichar cp = g_utf8_get_char((const char *)p);
            if (cp <= 0xFFFF) g_string_append_printf(out, "\\u%04x", cp);
            else {
                uint32_t u = cp - 0x10000;
                g_string_append_printf(out, "\\u%04x\\u%04x", 0xD800 + (u >> 10), 0xDC00 + (u & 0x3FF));
            }
            p = (const unsigned char *)next;
        }
    }
    g_string_append_c(out, '"');
}

static void identity_hash(const SocketKey *key, const char *pane, char hex[65]) {
    GString *json = g_string_new("[");
    append_json_string(json, key && key->path ? key->path : "");
    g_string_append_printf(json, ", %llu, %llu, ",
        (unsigned long long)(key ? key->device : 0),
        (unsigned long long)(key ? key->inode : 0));
    append_json_string(json, pane ? pane : "");
    g_string_append_c(json, ']');
    uint8_t dig[32];
    sha256((const uint8_t *)json->str, json->len, dig);
    g_string_free(json, TRUE);
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", dig[i]);
    hex[64] = 0;
}

static char *join_pieces(char **parts, int count) {
    GString *out = g_string_new("");
    for (int i = 0; i < count; i++) {
        if (!parts[i] || !parts[i][0]) continue;
        if (out->len) g_string_append(out, " · ");
        g_string_append(out, parts[i]);
    }
    return g_string_free(out, FALSE);
}

static int split_agent(const char *agent, char **parts, int max) {
    int n = 0;
    GString *cur = g_string_new("");
    const char *p = agent ? agent : "";
    while (*p && n < max) {
        const char *next = g_utf8_next_char(p);
        gunichar c = next == p ? (unsigned char)*p : g_utf8_get_char(p);
        if (c == 0x00B7) {
            char *piece = labels_clean(cur->str);
            parts[n++] = piece ? piece : xdup("");
            g_string_truncate(cur, 0);
        } else {
            g_string_append_unichar(cur, c);
        }
        p = next == p ? p + 1 : next;
    }
    if (n < max) {
        char *piece = labels_clean(cur->str);
        parts[n++] = piece ? piece : xdup("");
    }
    g_string_free(cur, TRUE);
    return n;
}

static const char *short_pane(const char *pane) {
    const char *colon = pane ? strrchr(pane, ':') : NULL;
    return colon ? colon + 1 : (pane ? pane : "");
}

static int count_text(char **values, size_t n, const char *text) {
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (values[i] && text && strcmp(values[i], text) == 0) count++;
    }
    return count;
}

static void entry_free_fields(LabelEntry *entry) {
    if (!entry) return;
    free(entry->harness);
    free(entry->pane);
    free(entry->token);
    free(entry->id);
    free(entry->label);
    free(entry->full_label);
    free(entry->thinking);
    free(entry->model);
    free(entry->model_id);
    free(entry->model_name);
    socket_key_clear(&entry->socket_key);
    memset(entry, 0, sizeof *entry);
}

void labels_entry_clear(LabelEntry *entry) {
    entry_free_fields(entry);
}

void labels_entries_free(LabelEntry *entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) entry_free_fields(&entries[i]);
    free(entries);
}

static int entry_copy(LabelEntry *dst, const LabelEntry *src) {
    memset(dst, 0, sizeof *dst);
    dst->harness = xdup(src->harness);
    dst->pane = xdup(src->pane);
    dst->token = xdup(src->token);
    dst->id = xdup(src->id);
    dst->thinking = xdup(src->thinking);
    dst->model = xdup(src->model);
    dst->model_id = xdup(src->model_id);
    dst->model_name = xdup(src->model_name);
    dst->label = src->label ? xdup(src->label) : NULL;
    dst->full_label = src->full_label ? xdup(src->full_label) : NULL;
    dst->has_model = src->has_model;
    dst->model_is_dict = src->model_is_dict;
    dst->selected = src->selected;
    dst->team_child = src->team_child;
    dst->label_present = src->label_present;
    dst->full_label_present = src->full_label_present;
    dst->has_socket_key = src->has_socket_key;
    if (src->has_socket_key && socket_key_copy(&dst->socket_key, &src->socket_key) != LABELS_OK) return LABELS_ERR;
    if (!dst->harness || !dst->pane || !dst->token || !dst->id || !dst->thinking || !dst->model || !dst->model_id || !dst->model_name)
        return LABELS_ERR;
    return LABELS_OK;
}

static const SnapshotState *find_snapshot(const SocketKey *key, const SocketKey *keys, SnapshotState *const *states, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (keys && states && states[i] && socket_key_equal(&keys[i], key)) return states[i];
    }
    return NULL;
}

int labels_build(const LabelEntry *in, size_t count,
    const SocketKey *snap_keys, SnapshotState *const *snap_states, size_t snap_count,
    LabelEntry **out, size_t *out_count) {
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    if (!out || !out_count || (count && !in)) return LABELS_ERR;
    LabelEntry *rows = calloc(count ? count : 1, sizeof *rows);
    if (!rows) return LABELS_ERR;
    for (size_t i = 0; i < count; i++) {
        if (entry_copy(&rows[i], &in[i]) != LABELS_OK) {
            labels_entries_free(rows, count);
            return LABELS_ERR;
        }
    }
    typedef struct {
        size_t index;
        char *pane;
        char *workspace;
        char *tab;
        char *pane_name;
        char *model;
        char *thinking;
        char *body;
        char hash[65];
        int named;
        int selected;
        int team;
        SocketKey key;
    } Prep;
    Prep *prep = NULL;
    size_t np = 0;
    int rc = LABELS_OK;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(rows[i].harness, "pi") != 0 && strcmp(rows[i].harness, "qwen-pi") != 0) continue;
        char *pane = labels_clean(rows[i].pane);
        if (!pane || !pane[0]) {
            set_error("Pi label requires a full pane identity");
            free(pane);
            rc = LABELS_INVALID;
            break;
        }
        if (!rows[i].has_socket_key) {
            set_error("Pi label requires a socket key");
            free(pane);
            rc = LABELS_INVALID;
            break;
        }
        Prep *grown = realloc(prep, (np + 1) * sizeof *prep);
        if (!grown) {
            free(pane);
            rc = LABELS_ERR;
            break;
        }
        prep = grown;
        memset(&prep[np], 0, sizeof prep[np]);
        prep[np].index = i;
        prep[np].pane = pane;
        if (socket_key_copy(&prep[np].key, &rows[i].socket_key) != LABELS_OK) {
            rc = LABELS_ERR;
            break;
        }
        const SnapshotState *state = find_snapshot(&rows[i].socket_key, snap_keys, snap_states, snap_count);
        ssize_t slot = state_find(state, rows[i].pane);
        char *workspace = labels_clean(slot >= 0 ? state->workspace[slot] : "");
        char *tab = labels_clean(slot >= 0 ? state->tab[slot] : "");
        char *pane_name = labels_clean(slot >= 0 ? state->pane[slot] : "");
        char *agent = labels_clean(slot >= 0 ? state->display_agent[slot] : "");
        const char *raw_model = "";
        if (rows[i].model_is_dict) {
            if (rows[i].model_id && rows[i].model_id[0]) raw_model = rows[i].model_id;
            else if (rows[i].model_name && rows[i].model_name[0]) raw_model = rows[i].model_name;
        } else if (rows[i].has_model) {
            raw_model = rows[i].model ? rows[i].model : "";
        }
        char *model = labels_clean(raw_model);
        char *thinking = labels_clean(rows[i].thinking);
        char *parts[8] = {0};
        int part_count = split_agent(agent, parts, 8);
        if (part_count == 3 && (strcmp(parts[0], "pi") == 0 || strcmp(parts[0], "qwen-pi") == 0)) {
            if (!thinking[0]) {
                free(thinking);
                thinking = xdup(parts[1]);
            }
            if (!model[0]) {
                free(model);
                model = xdup(parts[2]);
            }
        }
        for (int p = 0; p < part_count; p++) free(parts[p]);
        free(agent);
        prep[np].workspace = workspace;
        prep[np].tab = tab;
        prep[np].pane_name = pane_name;
        prep[np].model = model;
        prep[np].thinking = thinking;
        prep[np].selected = rows[i].selected;
        prep[np].team = rows[i].team_child;
        prep[np].named = (workspace && workspace[0]) || (tab && tab[0]) || (pane_name && pane_name[0]);
        char *pieces[8];
        int pc = 0;
        pieces[pc++] = "pi";
        if (workspace && workspace[0]) pieces[pc++] = workspace;
        if (tab && tab[0] && strcmp(tab, workspace ? workspace : "") != 0) pieces[pc++] = tab;
        if (pane_name && pane_name[0] && strcmp(pane_name, workspace ? workspace : "") != 0
            && strcmp(pane_name, tab ? tab : "") != 0) pieces[pc++] = pane_name;
        if (thinking && thinking[0]) pieces[pc++] = thinking;
        if (model && model[0]) pieces[pc++] = model;
        prep[np].body = join_pieces(pieces, pc);
        identity_hash(&prep[np].key, prep[np].pane, prep[np].hash);
        if (!workspace || !tab || !pane_name || !model || !thinking || !prep[np].body) rc = LABELS_ERR;
        np++;
        if (rc != LABELS_OK) break;
    }
    if (rc == LABELS_OK && prep) {
        int digest = 6;
        for (;;) {
            int collision = 0;
            for (size_t i = 0; i < np && !collision; i++) {
                for (size_t j = i + 1; j < np; j++) {
                    int same = socket_key_equal(&prep[i].key, &prep[j].key) && strcmp(prep[i].pane, prep[j].pane) == 0;
                    if (same) continue;
                    if (strncmp(prep[i].hash, prep[j].hash, (size_t)digest) == 0) {
                        collision = 1;
                        break;
                    }
                }
            }
            if (!collision || digest >= 64) break;
            digest++;
        }
        char **bodies = calloc(np ? np : 1, sizeof *bodies);
        char **shorts = calloc(np ? np : 1, sizeof *shorts);
        char **panes = calloc(np ? np : 1, sizeof *panes);
        int *identified = calloc(np ? np : 1, sizeof *identified);
        if (!bodies || !shorts || !panes || !identified) rc = LABELS_ERR;
        for (size_t i = 0; rc == LABELS_OK && i < np; i++) {
            bodies[i] = prep[i].body;
            shorts[i] = (char *)short_pane(prep[i].pane);
            panes[i] = prep[i].pane;
        }
        for (size_t i = 0; rc == LABELS_OK && i < np; i++) {
            if (!prep[i].named || count_text(bodies, np, prep[i].body) > 1) identified[i] = 1;
        }
        while (rc == LABELS_OK) {
            for (size_t i = 0; i < np; i++) {
                char flags[2][16];
                char *suffix[4];
                int ns = 0;
                if (prep[i].selected) {
                    snprintf(flags[0], sizeof flags[0], "selected");
                    suffix[ns++] = flags[0];
                }
                if (prep[i].team) {
                    snprintf(flags[1], sizeof flags[1], "team");
                    suffix[ns++] = flags[1];
                }
                char *identity = NULL;
                if (identified[i]) {
                    const char *base = prep[i].pane;
                    if (prep[i].named && count_text(shorts, np, short_pane(prep[i].pane)) <= 1)
                        base = short_pane(prep[i].pane);
                    if (count_text(panes, np, prep[i].pane) > 1) {
                        size_t n = strlen(base) + 1 + (size_t)digest + 1;
                        identity = malloc(n);
                        if (identity) snprintf(identity, n, "%s@%.*s", base, digest, prep[i].hash);
                    } else {
                        identity = xdup(base);
                    }
                    if (!identity) {
                        rc = LABELS_ERR;
                        break;
                    }
                    suffix[ns++] = identity;
                }
                char *tail = NULL;
                if (ns == 0) tail = xdup("");
                else {
                    char *joined = join_pieces(suffix, ns);
                    size_t n = strlen(joined) + 8;
                    tail = malloc(n);
                    if (tail) snprintf(tail, n, " · %s", joined);
                    free(joined);
                }
                free(identity);
                if (!tail) {
                    rc = LABELS_ERR;
                    break;
                }
                char *full = malloc(strlen(prep[i].body) + strlen(tail) + 1);
                if (!full) {
                    free(tail);
                    rc = LABELS_ERR;
                    break;
                }
                sprintf(full, "%s%s", prep[i].body, tail);
                free(rows[prep[i].index].full_label);
                rows[prep[i].index].full_label = full;
                rows[prep[i].index].full_label_present = 1;
                if (labels_display_width(tail) > 50) {
                    free(tail);
                    char hash_part[80];
                    snprintf(hash_part, sizeof hash_part, "@%.*s", digest, prep[i].hash);
                    char *short_suffix[3];
                    int nss = 0;
                    if (prep[i].selected) short_suffix[nss++] = flags[0];
                    if (prep[i].team) short_suffix[nss++] = flags[1];
                    short_suffix[nss++] = hash_part;
                    char *joined = join_pieces(short_suffix, nss);
                    size_t n = strlen(joined) + 8;
                    tail = malloc(n);
                    if (tail) snprintf(tail, n, " · %s", joined);
                    free(joined);
                    if (!tail) {
                        rc = LABELS_ERR;
                        break;
                    }
                }
                int budget = LABELS_SHORT_BUDGET - labels_display_width(tail);
                if (budget < 0) budget = 0;
                char *body = truncate_text(prep[i].body, budget);
                if (!body) {
                    free(tail);
                    rc = LABELS_ERR;
                    break;
                }
                char *label = malloc(strlen(body) + strlen(tail) + 1);
                if (!label) {
                    free(body);
                    free(tail);
                    rc = LABELS_ERR;
                    break;
                }
                sprintf(label, "%s%s", body, tail);
                free(body);
                free(tail);
                free(rows[prep[i].index].label);
                rows[prep[i].index].label = label;
                rows[prep[i].index].label_present = 1;
            }
            if (rc != LABELS_OK) break;
            int grew = 0;
            for (size_t i = 0; i < np; i++) {
                if (identified[i]) continue;
                int hits = 0;
                for (size_t j = 0; j < np; j++) {
                    if (strcmp(rows[prep[i].index].label, rows[prep[j].index].label) == 0) hits++;
                }
                if (hits > 1) {
                    identified[i] = 1;
                    grew = 1;
                }
            }
            if (!grew) break;
        }
        free(bodies);
        free(shorts);
        free(panes);
        free(identified);
    }
    if (prep) {
        for (size_t i = 0; i < np; i++) {
            free(prep[i].pane);
            free(prep[i].workspace);
            free(prep[i].tab);
            free(prep[i].pane_name);
            free(prep[i].model);
            free(prep[i].thinking);
            free(prep[i].body);
            socket_key_clear(&prep[i].key);
        }
        free(prep);
    }
    if (rc != LABELS_OK) {
        labels_entries_free(rows, count);
        return rc;
    }
    *out = rows;
    *out_count = count;
    return LABELS_OK;
}
