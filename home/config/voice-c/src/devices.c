#define _POSIX_C_SOURCE 200809L
#include "devices.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct Json Json;

struct Json {
    int type;
    int bool_val;
    char *str;
    char **keys;
    Json *children;
    size_t n;
    size_t cap;
};

enum { JSON_NONE, JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ };

typedef struct Snap {
    char *name;
    char *target;
    int muted;
    int muted_known;
    char *preferred;
    int missing;
    char *error;
} Snap;

struct MicrophoneMonitor {
    char *preferred;
    double refresh_seconds;
    MicRunner runner;
    void *user;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    Snap snapshot;
    int has_update;
    double updated;
    int probe_alive;
    int probe_started;
    pthread_t probe_thread;
    int destroyed;
};

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void add_seconds(struct timespec *ts, double seconds) {
    time_t whole = (time_t)seconds;
    long nsec = (long)((seconds - (double)whole) * 1e9);
    ts->tv_sec += whole;
    ts->tv_nsec += nsec;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static char *dup_text(const char *text) {
    if (!text) return NULL;
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    return copy;
}

static void snap_clear(Snap *snap) {
    free(snap->name);
    free(snap->target);
    free(snap->preferred);
    free(snap->error);
    memset(snap, 0, sizeof *snap);
}

static int snap_unknown(Snap *snap, const char *preferred, const char *error) {
    snap_clear(snap);
    snap->name = dup_text("System default");
    snap->preferred = preferred && preferred[0] ? dup_text(preferred) : NULL;
    snap->missing = snap->preferred != NULL;
    snap->error = error ? dup_text(error) : NULL;
    return snap->name ? 0 : -1;
}

static int snap_copy(const Snap *snap, MicStatus *out) {
    memset(out, 0, sizeof *out);
    out->name = dup_text(snap->name);
    out->target = dup_text(snap->target);
    out->preferred = dup_text(snap->preferred);
    out->error = dup_text(snap->error);
    out->muted = snap->muted;
    out->muted_known = snap->muted_known;
    out->missing = snap->missing;
    if ((snap->name && !out->name) || (snap->target && !out->target)
        || (snap->preferred && !out->preferred) || (snap->error && !out->error)) {
        mic_status_free(out);
        return -1;
    }
    return 0;
}

void mic_status_free(MicStatus *status) {
    if (!status) return;
    free(status->name);
    free(status->target);
    free(status->preferred);
    free(status->error);
    memset(status, 0, sizeof *status);
}

static void json_free(Json *json) {
    if (!json) return;
    free(json->str);
    for (size_t i = 0; i < json->n; i++) {
        if (json->keys) free(json->keys[i]);
        json_free(&json->children[i]);
    }
    free(json->keys);
    free(json->children);
    memset(json, 0, sizeof *json);
}

static int json_push(Json *parent, const char *key, Json *child) {
    if (parent->n == parent->cap) {
        size_t cap = parent->cap ? parent->cap * 2 : 4;
        Json *children = realloc(parent->children, cap * sizeof *children);
        if (!children) return -1;
        parent->children = children;
        if (parent->type == JSON_OBJ) {
            char **keys = realloc(parent->keys, cap * sizeof *keys);
            if (!keys) return -1;
            parent->keys = keys;
        }
        parent->cap = cap;
    }
    if (parent->type == JSON_OBJ) {
        parent->keys[parent->n] = dup_text(key ? key : "");
        if (!parent->keys[parent->n]) return -1;
    }
    parent->children[parent->n++] = *child;
    memset(child, 0, sizeof *child);
    return 0;
}

typedef struct {
    const char *p;
    const char *end;
} Parser;

static void skip_ws(Parser *p) {
    while (p->p < p->end) {
        char ch = *p->p;
        if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
        p->p++;
    }
}

static int parse_value(Parser *p, Json *out);

static int parse_string(Parser *p, char **out) {
    if (p->p >= p->end || *p->p != '"') return -1;
    p->p++;
    size_t cap = 32;
    size_t n = 0;
    char *text = malloc(cap);
    if (!text) return -1;
    while (p->p < p->end) {
        unsigned char ch = (unsigned char)*p->p++;
        if (ch == '"') {
            text[n] = '\0';
            *out = text;
            return 0;
        }
        if (ch == '\\') {
            if (p->p >= p->end) break;
            char esc = *p->p++;
            if (esc == 'u') {
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    if (p->p >= p->end) {
                        free(text);
                        return -1;
                    }
                    char hex = *p->p++;
                    code <<= 4;
                    if (hex >= '0' && hex <= '9') code += (unsigned)(hex - '0');
                    else if (hex >= 'a' && hex <= 'f') code += (unsigned)(hex - 'a' + 10);
                    else if (hex >= 'A' && hex <= 'F') code += (unsigned)(hex - 'A' + 10);
                    else {
                        free(text);
                        return -1;
                    }
                }
                if (code < 0x80) ch = (unsigned char)code;
                else if (code < 0x800) {
                    if (n + 2 >= cap) {
                        cap *= 2;
                        char *next = realloc(text, cap);
                        if (!next) {
                            free(text);
                            return -1;
                        }
                        text = next;
                    }
                    text[n++] = (char)(0xc0 | (code >> 6));
                    ch = (unsigned char)(0x80 | (code & 0x3f));
                } else {
                    if (n + 3 >= cap) {
                        cap *= 2;
                        char *next = realloc(text, cap);
                        if (!next) {
                            free(text);
                            return -1;
                        }
                        text = next;
                    }
                    text[n++] = (char)(0xe0 | (code >> 12));
                    text[n++] = (char)(0x80 | ((code >> 6) & 0x3f));
                    ch = (unsigned char)(0x80 | (code & 0x3f));
                }
            } else if (esc == '"' || esc == '\\' || esc == '/') ch = (unsigned char)esc;
            else if (esc == 'b') ch = '\b';
            else if (esc == 'f') ch = '\f';
            else if (esc == 'n') ch = '\n';
            else if (esc == 'r') ch = '\r';
            else if (esc == 't') ch = '\t';
            else {
                free(text);
                return -1;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *next = realloc(text, cap);
            if (!next) {
                free(text);
                return -1;
            }
            text = next;
        }
        text[n++] = (char)ch;
    }
    free(text);
    return -1;
}

static int parse_value(Parser *p, Json *out) {
    memset(out, 0, sizeof *out);
    skip_ws(p);
    if (p->p >= p->end) return -1;
    char ch = *p->p;
    if (ch == '"') {
        out->type = JSON_STR;
        return parse_string(p, &out->str);
    }
    if (ch == '{') {
        p->p++;
        out->type = JSON_OBJ;
        skip_ws(p);
        if (p->p < p->end && *p->p == '}') {
            p->p++;
            return 0;
        }
        while (p->p < p->end) {
            char *key = NULL;
            skip_ws(p);
            if (parse_string(p, &key) != 0) {
                json_free(out);
                return -1;
            }
            skip_ws(p);
            if (p->p >= p->end || *p->p != ':') {
                free(key);
                json_free(out);
                return -1;
            }
            p->p++;
            Json child;
            if (parse_value(p, &child) != 0) {
                free(key);
                json_free(out);
                return -1;
            }
            if (json_push(out, key, &child) != 0) {
                free(key);
                json_free(&child);
                json_free(out);
                return -1;
            }
            free(key);
            skip_ws(p);
            if (p->p < p->end && *p->p == ',') {
                p->p++;
                continue;
            }
            if (p->p < p->end && *p->p == '}') {
                p->p++;
                return 0;
            }
            json_free(out);
            return -1;
        }
        json_free(out);
        return -1;
    }
    if (ch == '[') {
        p->p++;
        out->type = JSON_ARR;
        skip_ws(p);
        if (p->p < p->end && *p->p == ']') {
            p->p++;
            return 0;
        }
        while (p->p < p->end) {
            Json child;
            if (parse_value(p, &child) != 0) {
                json_free(out);
                return -1;
            }
            if (json_push(out, NULL, &child) != 0) {
                json_free(&child);
                json_free(out);
                return -1;
            }
            skip_ws(p);
            if (p->p < p->end && *p->p == ',') {
                p->p++;
                continue;
            }
            if (p->p < p->end && *p->p == ']') {
                p->p++;
                return 0;
            }
            json_free(out);
            return -1;
        }
        json_free(out);
        return -1;
    }
    if (p->end - p->p >= 4 && strncmp(p->p, "true", 4) == 0) {
        p->p += 4;
        out->type = JSON_BOOL;
        out->bool_val = 1;
        return 0;
    }
    if (p->end - p->p >= 5 && strncmp(p->p, "false", 5) == 0) {
        p->p += 5;
        out->type = JSON_BOOL;
        out->bool_val = 0;
        return 0;
    }
    if (p->end - p->p >= 4 && strncmp(p->p, "null", 4) == 0) {
        p->p += 4;
        out->type = JSON_NULL;
        return 0;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        out->type = JSON_NUM;
        if (*p->p == '-') p->p++;
        while (p->p < p->end && ((*p->p >= '0' && *p->p <= '9') || *p->p == '.' || *p->p == 'e' || *p->p == 'E' || *p->p == '+' || *p->p == '-')) {
            p->p++;
        }
        return 0;
    }
    return -1;
}

static Json *json_get(Json *json, const char *key) {
    if (!json || json->type != JSON_OBJ) return NULL;
    for (size_t i = 0; i < json->n; i++) {
        if (json->keys[i] && strcmp(json->keys[i], key) == 0) return &json->children[i];
    }
    return NULL;
}

static const char *json_str(Json *json) {
    if (!json || json->type != JSON_STR) return NULL;
    return json->str;
}

static int truthy(const char *text) {
    return text && text[0];
}

typedef struct {
    char *name;
    char *description;
    char *nick;
    int muted;
    int muted_known;
} Source;

static int source_add(Source **sources, size_t *count, size_t *cap, Source item) {
    if (*count == *cap) {
        size_t next = *cap ? *cap * 2 : 4;
        Source *grown = realloc(*sources, next * sizeof *grown);
        if (!grown) return -1;
        *sources = grown;
        *cap = next;
    }
    (*sources)[(*count)++] = item;
    return 0;
}

static Source *find_source(Source *sources, size_t count, const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (sources[i].name && strcmp(sources[i].name, name) == 0) return &sources[i];
    }
    return NULL;
}

static void free_sources(Source *sources, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(sources[i].name);
        free(sources[i].description);
        free(sources[i].nick);
    }
    free(sources);
}

static int muted_from(Json *info, int *muted, int *known) {
    *known = 0;
    *muted = 0;
    Json *params = json_get(info, "params");
    if (!params) return 0;
    /* A present non-object is a parse error, matching Python's .get chain. */
    if (params->type != JSON_OBJ) return -1;
    Json *props = json_get(params, "Props");
    if (!props) return 0;
    if (props->type != JSON_ARR) return -1;
    for (size_t i = 0; i < props->n; i++) {
        Json *mute = json_get(&props->children[i], "mute");
        if (!mute) continue;
        if (mute->type != JSON_BOOL) return -1;
        *known = 1;
        *muted = mute->bool_val;
        return 0;
    }
    return 0;
}

static int parse_dump(const char *text, const char *preferred, Snap *out) {
    Parser parser = {text, text + strlen(text)};
    Json root;
    if (parse_value(&parser, &root) != 0 || root.type != JSON_ARR) {
        json_free(&root);
        return -1;
    }
    skip_ws(&parser);
    if (parser.p != parser.end) {
        json_free(&root);
        return -1;
    }
    Source *sources = NULL;
    size_t count = 0;
    size_t cap = 0;
    char *fallback = NULL;
    int failed = 0;
    for (size_t i = 0; i < root.n && !failed; i++) {
        Json *row = &root.children[i];
        Json *info = json_get(row, "info");
        Json *props = json_get(info, "props");
        const char *media = json_str(json_get(props, "media.class"));
        if (media && strcmp(media, "Audio/Source") == 0) {
            const char *name = json_str(json_get(props, "node.name"));
            if (name) {
                Source item;
                memset(&item, 0, sizeof item);
                item.name = dup_text(name);
                const char *description = json_str(json_get(props, "node.description"));
                const char *nick = json_str(json_get(props, "node.nick"));
                item.description = dup_text(description);
                item.nick = dup_text(nick);
                if (muted_from(info, &item.muted, &item.muted_known) != 0 || !item.name) {
                    free(item.name);
                    free(item.description);
                    free(item.nick);
                    failed = 1;
                } else {
                    /* Python stores sources in a dict, so the last name wins. */
                    Source *existing = find_source(sources, count, name);
                    if (existing) {
                        free(existing->name);
                        free(existing->description);
                        free(existing->nick);
                        *existing = item;
                    } else if (source_add(&sources, &count, &cap, item) != 0) {
                        free(item.name);
                        free(item.description);
                        free(item.nick);
                        failed = 1;
                    }
                }
            }
        }
        Json *row_props = json_get(row, "props");
        const char *meta_name = json_str(json_get(row_props, "metadata.name"));
        if (!meta_name || strcmp(meta_name, "default") != 0) continue;
        Json *metadata = json_get(row, "metadata");
        if (!metadata) continue;
        if (metadata->type != JSON_ARR) {
            failed = 1;
            break;
        }
        for (size_t m = 0; m < metadata->n && !failed; m++) {
            const char *key = json_str(json_get(&metadata->children[m], "key"));
            if (!key || strcmp(key, "default.audio.source") != 0) continue;
            Json *value = json_get(&metadata->children[m], "value");
            Json nested = {0};
            Json *object = value;
            int skip_name = !value || value->type == JSON_NULL
                || (value->type == JSON_BOOL && !value->bool_val)
                || (value->type == JSON_ARR && value->n == 0)
                || (value->type == JSON_STR && (!value->str || !value->str[0]));
            if (!skip_name && value->type == JSON_STR) {
                Parser nested_parser = {value->str, value->str + strlen(value->str)};
                if (parse_value(&nested_parser, &nested) != 0) {
                    failed = 1;
                    break;
                }
                skip_ws(&nested_parser);
                if (nested_parser.p != nested_parser.end || nested.type != JSON_OBJ) {
                    json_free(&nested);
                    failed = 1;
                    break;
                }
                object = &nested;
            } else if (!skip_name && value->type != JSON_OBJ) {
                failed = 1;
                break;
            }
            const char *name = skip_name ? NULL : json_str(json_get(object, "name"));
            free(fallback);
            fallback = dup_text(name);
            json_free(&nested);
        }
    }
    json_free(&root);
    if (failed) {
        free_sources(sources, count);
        free(fallback);
        return -1;
    }
    const char *chosen_name = NULL;
    if (preferred && preferred[0] && find_source(sources, count, preferred)) chosen_name = preferred;
    else chosen_name = fallback;
    Source *chosen = find_source(sources, count, chosen_name);
    if (!chosen) {
        free_sources(sources, count);
        free(fallback);
        return snap_unknown(out, preferred, "No default microphone available");
    }
    const char *label = truthy(chosen->description) ? chosen->description
        : truthy(chosen->nick) ? chosen->nick : chosen->name;
    snap_clear(out);
    out->name = dup_text(label);
    if (preferred && preferred[0] && chosen->name && strcmp(chosen->name, preferred) == 0) {
        out->target = dup_text(chosen->name);
    }
    out->muted = chosen->muted;
    out->muted_known = chosen->muted_known;
    out->preferred = preferred && preferred[0] ? dup_text(preferred) : NULL;
    out->missing = out->preferred && !(out->target);
    int bad = !out->name || (preferred && preferred[0] && !out->preferred);
    free_sources(sources, count);
    free(fallback);
    if (bad) return -1;
    return 0;
}

static int publish_unknown(MicrophoneMonitor *monitor, const char *error) {
    Snap next;
    memset(&next, 0, sizeof next);
    if (snap_unknown(&next, monitor->preferred, error) != 0) return -1;
    snap_clear(&monitor->snapshot);
    monitor->snapshot = next;
    monitor->updated = mono();
    monitor->has_update = 1;
    return 0;
}

static void *probe_main(void *arg) {
    MicrophoneMonitor *monitor = arg;
    MicRunResult result;
    memset(&result, 0, sizeof result);
    char *argv[] = {"pw-dump", NULL};
    Snap next;
    memset(&next, 0, sizeof next);
    int failed = monitor->runner(argv, 2.0, &result, monitor->user) != 0
        || !result.ok || result.exit_code != 0 || !result.stdout_text;
    if (!failed) failed = parse_dump(result.stdout_text, monitor->preferred, &next) != 0;
    free(result.stdout_text);
    pthread_mutex_lock(&monitor->lock);
    if (monitor->destroyed) {
        pthread_mutex_unlock(&monitor->lock);
        snap_clear(&next);
        return NULL;
    }
    if (failed) {
        publish_unknown(monitor, "Microphone details unavailable");
        snap_clear(&next);
    } else {
        snap_clear(&monitor->snapshot);
        monitor->snapshot = next;
        monitor->updated = mono();
        monitor->has_update = 1;
    }
    monitor->probe_alive = 0;
    pthread_cond_broadcast(&monitor->cv);
    pthread_mutex_unlock(&monitor->lock);
    return NULL;
}

static int start_probe(MicrophoneMonitor *monitor) {
    if (monitor->probe_alive) return 0;
    /* Join the finished probe before replacing its thread id. probe_main
       clears probe_alive and unlocks before it returns, and it does not lock
       again, so joining here while holding the mutex cannot deadlock. A later
       pthread_create failure must not wipe a good snapshot. */
    if (monitor->probe_started) {
        pthread_t finished = monitor->probe_thread;
        monitor->probe_started = 0;
        pthread_join(finished, NULL);
    }
    if (monitor->probe_alive || monitor->destroyed) return 0;
    monitor->probe_alive = 1;
    if (pthread_create(&monitor->probe_thread, NULL, probe_main, monitor) != 0) {
        monitor->probe_alive = 0;
        return -1;
    }
    monitor->probe_started = 1;
    return 0;
}

MicrophoneMonitor *mic_monitor_new(const char *preferred, MicRunner runner, void *user) {
    MicrophoneMonitor *monitor = calloc(1, sizeof *monitor);
    if (!monitor) return NULL;
    if (preferred && preferred[0]) {
        monitor->preferred = dup_text(preferred);
        if (!monitor->preferred) {
            free(monitor);
            return NULL;
        }
    }
    monitor->refresh_seconds = 5.0;
    monitor->runner = runner ? runner : mic_runner_pw_dump;
    monitor->user = user;
    pthread_mutex_init(&monitor->lock, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&monitor->cv, &attr);
    pthread_condattr_destroy(&attr);
    snap_unknown(&monitor->snapshot, monitor->preferred, NULL);
    return monitor;
}

void mic_monitor_free(MicrophoneMonitor *monitor) {
    if (!monitor) return;
    pthread_mutex_lock(&monitor->lock);
    while (monitor->probe_alive) pthread_cond_wait(&monitor->cv, &monitor->lock);
    int started = monitor->probe_started;
    pthread_t thread = monitor->probe_thread;
    monitor->destroyed = 1;
    pthread_mutex_unlock(&monitor->lock);
    if (started) pthread_join(thread, NULL);
    snap_clear(&monitor->snapshot);
    free(monitor->preferred);
    pthread_cond_destroy(&monitor->cv);
    pthread_mutex_destroy(&monitor->lock);
    free(monitor);
}

int mic_status(MicrophoneMonitor *monitor, MicStatus *out) {
    if (!monitor || !out) return -1;
    pthread_mutex_lock(&monitor->lock);
    if (!monitor->has_update || mono() - monitor->updated >= monitor->refresh_seconds) {
        start_probe(monitor);
    }
    int rc = snap_copy(&monitor->snapshot, out);
    pthread_mutex_unlock(&monitor->lock);
    return rc;
}

int mic_resolve(MicrophoneMonitor *monitor, MicStatus *out) {
    if (!monitor || !out) return -1;
    pthread_mutex_lock(&monitor->lock);
    start_probe(monitor);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    add_seconds(&ts, 2.2);
    while (monitor->probe_alive) {
        int rc = pthread_cond_timedwait(&monitor->cv, &monitor->lock, &ts);
        if (rc == ETIMEDOUT) break;
    }
    int alive = monitor->probe_alive;
    int copy = 0;
    if (alive) {
        pthread_mutex_unlock(&monitor->lock);
        Snap unknown;
        memset(&unknown, 0, sizeof unknown);
        snap_unknown(&unknown, monitor->preferred, "Microphone details unavailable");
        copy = snap_copy(&unknown, out);
        snap_clear(&unknown);
        return copy;
    }
    copy = snap_copy(&monitor->snapshot, out);
    pthread_mutex_unlock(&monitor->lock);
    return copy;
}

int mic_runner_pw_dump(char *const *argv, double timeout, MicRunResult *out, void *user) {
    (void)user;
    memset(out, 0, sizeof *out);
    if (!argv || !argv[0]) return -1;
    const char *list[8];
    int argc = 0;
    for (; argv[argc] && argc < 8; argc++) list[argc] = argv[argc];
    if (argv[argc]) return -1;
    double seconds = timeout > 0 && timeout < 2.0 ? timeout : 2.0;
    ipc_process_request request;
    memset(&request, 0, sizeof request);
    request.argv = list;
    request.argc = argc;
    request.capture_stdout = 1;
    request.deadline_ms = (int)(seconds * 1000.0);
    if (request.deadline_ms < 1) request.deadline_ms = 1;
    request.stdout_max = 4u * 1024u * 1024u;
    ipc_process_result result;
    memset(&result, 0, sizeof result);
    char err[64];
    int rc = ipc_process_run(&request, &result, err, sizeof err);
    if (rc == IPC_TIMEOUT) {
        ipc_process_result_free(&result);
        out->ok = 0;
        out->exit_code = 124;
        return 0;
    }
    if (rc != IPC_OK) {
        ipc_process_result_free(&result);
        return -1;
    }
    out->exit_code = result.exit_code;
    out->ok = result.exit_code == 0 && result.stdout_bytes;
    if (out->ok) {
        out->stdout_text = malloc(result.stdout_len + 1);
        if (!out->stdout_text) {
            ipc_process_result_free(&result);
            return -1;
        }
        if (result.stdout_len) memcpy(out->stdout_text, result.stdout_bytes, result.stdout_len);
        out->stdout_text[result.stdout_len] = '\0';
    }
    ipc_process_result_free(&result);
    return 0;
}
