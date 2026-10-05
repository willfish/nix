#define _GNU_SOURCE
#include "audio.h"
#include "chunks.h"

#include <yyjson.h>
#include <curl/curl.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    LISTEN_MAX = 1024 * 1024,
    TTS_MAX = 32 * 1024 * 1024
};

extern char **environ;

typedef struct voice_choice {
    const audio_backend *backend;
    char model[128];
    size_t context_words;
} voice_choice;

struct audio {
    char *runtime;
    audio_backend backends[4];
    size_t backend_count;
    char selected_voices[4][64];
    char *stt_prompt;
    char *stt_language;
    char *preferred_microphone;
    char *speech_preferences;
    char *stt_preferences;
    char *playback_mode;
    char speech_backend[32];
    char stt_backend[32];
    char stt_state[32];
    char tts_state[32];
    char stt_error[512];
    char tts_error[512];
    int has_stt_error;
    int has_tts_error;
    audio_http_fn http;
    void *http_user;
    audio_popen_fn popen_fn;
    void *popen_user;
    audio_run_fn run_fn;
    void *run_user;
    audio_audible_fn audible;
    void *audible_user;
    audio_mic_status_fn mic_status;
    audio_mic_resolve_fn mic_resolve;
    audio_mic_details_fn mic_details;
    void *mic_user;
    void *mic_details_user;
    audio_capture_fn capture;
    void *capture_user;
    audio_player *player;
    atomic_int stop_gen;
    int live;
    pthread_mutex_t mu;
    pthread_mutex_t backend_mu;
    pthread_mutex_t synthesis_mu;
    pthread_mutex_t recognition_mu;
    pthread_mutex_t call_mu;
    pthread_mutex_t live_mu;
    pthread_cond_t live_cv;
};

typedef struct op_result {
    char *text;
    unsigned char *frames;
    size_t frame_len;
    int channels;
    int width;
    int rate;
    char error[512];
    int status;
} op_result;

typedef struct job {
    audio *audio;
    pthread_mutex_t *gate;
    atomic_int *cancelled;
    int stop_gen;
    int (*op)(void *user, op_result *result);
    void *user;
    void (*free_user)(void *user);
    audio_drained_fn on_drained;
    void *drain_user;
    op_result result;
    int done;
    int left;
    void (*destroy_ctx)(void *);
    void *destroy_arg;
    atomic_int destroy_done;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} job;

static void destroy_job_ctx(job *job) {
    if (!job || !job->destroy_ctx) return;
    if (atomic_exchange(&job->destroy_done, 1) == 0) job->destroy_ctx(job->destroy_arg);
}

typedef enum { JSON_NULL, JSON_BOOL, JSON_STRING, JSON_ARRAY, JSON_OBJECT, JSON_NUMBER } json_type;

typedef struct json_value {
    json_type type;
    int bool_value;
    char *string;
    struct json_value **items;
    size_t nitems;
    char **keys;
    struct json_value **members;
    size_t nmembers;
} json_value;

static void set_err(char *err, size_t cap, const char *msg) {
    if (!err || !cap) return;
    snprintf(err, cap, "%s", msg ? msg : "");
}

static char *dup_opt(const char *text) {
    if (!text || !text[0]) return NULL;
    return strdup(text);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_file(const char *path, unsigned char **out, size_t *len) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return -1; }
    long size = ftell(file);
    if (size < 0) { fclose(file); return -1; }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return -1; }
    unsigned char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(file); return -1; }
    if (size && fread(buf, 1, (size_t)size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return -1;
    }
    fclose(file);
    buf[size] = 0;
    *out = buf;
    *len = (size_t)size;
    return 0;
}

static void strip(char *text) {
    if (!text) return;
    char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
}

static char *sibling_name(const char *path, const char *name) {
    if (!path || !name) return NULL;
    const char *slash = strrchr(path, '/');
    size_t dir = slash ? (size_t)(slash - path + 1) : 0;
    char *out = malloc(dir + strlen(name) + 1);
    if (!out) return NULL;
    memcpy(out, path, dir);
    memcpy(out + dir, name, strlen(name) + 1);
    return out;
}

static char *with_suffix(const char *path, const char *suffix) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t keep = dot && dot != base ? (size_t)(dot - path) : strlen(path);
    char *out = malloc(keep + strlen(suffix) + 1);
    if (!out) return NULL;
    memcpy(out, path, keep);
    memcpy(out + keep, suffix, strlen(suffix) + 1);
    return out;
}

static int save_pref(const char *path, const char *value) {
    if (!path || !path[0]) return 0;
    char *tmp = with_suffix(path, ".tmp");
    if (!tmp) return -1;
    FILE *file = fopen(tmp, "w");
    if (!file) { free(tmp); return -1; }
    if (fprintf(file, "%s\n", value) < 0) {
        fclose(file);
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (fclose(file) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

static char *read_pref(const char *path) {
    if (!path) return NULL;
    FILE *file = fopen(path, "r");
    if (!file) return NULL;
    char buf[256];
    if (!fgets(buf, sizeof buf, file)) { fclose(file); return NULL; }
    fclose(file);
    strip(buf);
    return strdup(buf);
}

static int backend_available(const audio_backend *backend) {
    if (!backend->auth_env) return 1;
    const char *key = getenv(backend->auth_env);
    return key && key[0];
}

static int backend_index(const audio *audio, const char *id, int speech) {
    if (!id) return -1;
    for (size_t i = 0; i < audio->backend_count; i++) {
        const audio_backend *backend = &audio->backends[i];
        if (strcmp(backend->id, id) == 0 && (speech ? backend->speak_url : backend->listen_url)) return (int)i;
    }
    return -1;
}

static int voice_index(const audio_backend *backend, const char *id) {
    if (!id) return -1;
    for (size_t i = 0; i < backend->voice_count; i++)
        if (strcmp(backend->voices[i].id, id) == 0) return (int)i;
    return -1;
}

static void set_backend(audio *audio, const char *engine, const char *state, const char *error) {
    pthread_mutex_lock(&audio->backend_mu);
    char *slot = strcmp(engine, "stt") == 0 ? audio->stt_state : audio->tts_state;
    char *err = strcmp(engine, "stt") == 0 ? audio->stt_error : audio->tts_error;
    int *has = strcmp(engine, "stt") == 0 ? &audio->has_stt_error : &audio->has_tts_error;
    snprintf(slot, 32, "%s", state);
    if (error && error[0]) {
        snprintf(err, 512, "%s", error);
        *has = 1;
    } else {
        err[0] = 0;
        *has = 0;
    }
    pthread_mutex_unlock(&audio->backend_mu);
}

static void json_free(json_value *value) {
    if (!value) return;
    free(value->string);
    for (size_t i = 0; i < value->nitems; i++) json_free(value->items[i]);
    free(value->items);
    for (size_t i = 0; i < value->nmembers; i++) {
        free(value->keys[i]);
        json_free(value->members[i]);
    }
    free(value->keys);
    free(value->members);
    free(value);
}

static json_value *json_new(json_type type) {
    json_value *value = calloc(1, sizeof *value);
    if (value) value->type = type;
    return value;
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static int utf8_append(char **out, size_t *n, size_t *cap, unsigned code) {
    unsigned char bytes[4];
    int len = 0;
    if (code < 0x80) { bytes[0] = (unsigned char)code; len = 1; }
    else if (code < 0x800) {
        bytes[0] = (unsigned char)(0xC0 | (code >> 6));
        bytes[1] = (unsigned char)(0x80 | (code & 0x3F));
        len = 2;
    } else if (code < 0x10000) {
        if (code >= 0xD800 && code <= 0xDFFF) return -1;
        bytes[0] = (unsigned char)(0xE0 | (code >> 12));
        bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (code & 0x3F));
        len = 3;
    } else if (code <= 0x10FFFF) {
        bytes[0] = (unsigned char)(0xF0 | (code >> 18));
        bytes[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (code & 0x3F));
        len = 4;
    } else return -1;
    if (*n + (size_t)len + 1 > *cap) {
        size_t next = *cap ? *cap * 2 : 32;
        while (next < *n + (size_t)len + 1) next *= 2;
        char *grown = realloc(*out, next);
        if (!grown) return -1;
        *out = grown;
        *cap = next;
    }
    memcpy(*out + *n, bytes, (size_t)len);
    *n += (size_t)len;
    (*out)[*n] = 0;
    return 0;
}

static json_value *parse_value(const char **cursor, const char *end, int depth);

static int hex_digit(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

static int parse_hex4(const char **cursor, const char *end, unsigned *code) {
    if (end - *cursor < 4) return -1;
    unsigned value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hex_digit((*cursor)[i]);
        if (digit < 0) return -1;
        value = (value << 4) | (unsigned)digit;
    }
    *cursor += 4;
    *code = value;
    return 0;
}

static int append_raw_byte(char **out, size_t *n, size_t *cap, unsigned char byte) {
    if (*n + 2 > *cap) {
        size_t next = *cap ? *cap * 2 : 32;
        while (next < *n + 2) next *= 2;
        char *grown = realloc(*out, next);
        if (!grown) return -1;
        *out = grown;
        *cap = next;
    }
    (*out)[(*n)++] = (char)byte;
    (*out)[*n] = 0;
    return 0;
}

static json_value *parse_string(const char **cursor, const char *end) {
    const char *p = *cursor;
    if (p >= end || *p != '"') return NULL;
    p++;
    char *out = NULL;
    size_t n = 0, cap = 0;
    while (p < end && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            if (p >= end) { free(out); return NULL; }
            char esc = *p++;
            if (esc == 'u') {
                unsigned code = 0;
                if (parse_hex4(&p, end, &code) != 0) { free(out); return NULL; }
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u') { free(out); return NULL; }
                    p += 2;
                    unsigned low = 0;
                    if (parse_hex4(&p, end, &low) != 0 || low < 0xDC00 || low > 0xDFFF) {
                        free(out);
                        return NULL;
                    }
                    code = 0x10000u + (((code - 0xD800u) << 10) | (low - 0xDC00u));
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    free(out);
                    return NULL;
                }
                if (utf8_append(&out, &n, &cap, code) != 0) { free(out); return NULL; }
                continue;
            }
            if (esc == '"' || esc == '\\' || esc == '/') c = (unsigned char)esc;
            else if (esc == 'b') c = '\b';
            else if (esc == 'f') c = '\f';
            else if (esc == 'n') c = '\n';
            else if (esc == 'r') c = '\r';
            else if (esc == 't') c = '\t';
            else { free(out); return NULL; }
        }
        if (append_raw_byte(&out, &n, &cap, c) != 0) { free(out); return NULL; }
    }
    if (p >= end || *p != '"') { free(out); return NULL; }
    if (!out && utf8_append(&out, &n, &cap, 0) != 0) return NULL;
    json_value *value = json_new(JSON_STRING);
    if (!value) { free(out); return NULL; }
    value->string = out ? out : strdup("");
    *cursor = p + 1;
    return value;
}

static json_value *parse_value(const char **cursor, const char *end, int depth) {
    if (depth > 16) return NULL;
    const char *p = skip_ws(*cursor, end);
    if (p >= end) return NULL;
    if (*p == '"') {
        *cursor = p;
        return parse_string(cursor, end);
    }
    if (*p == '{') {
        json_value *obj = json_new(JSON_OBJECT);
        if (!obj) return NULL;
        p++;
        p = skip_ws(p, end);
        if (p < end && *p == '}') { *cursor = p + 1; return obj; }
        for (;;) {
            *cursor = p;
            json_value *key = parse_string(cursor, end);
            if (!key) { json_free(obj); return NULL; }
            p = skip_ws(*cursor, end);
            if (p >= end || *p != ':') { json_free(key); json_free(obj); return NULL; }
            p++;
            *cursor = p;
            json_value *member = parse_value(cursor, end, depth + 1);
            if (!member) { json_free(key); json_free(obj); return NULL; }
            char **keys = realloc(obj->keys, (obj->nmembers + 1) * sizeof *keys);
            json_value **members = realloc(obj->members, (obj->nmembers + 1) * sizeof *members);
            if (!keys || !members) {
                free(keys);
                free(members);
                json_free(key);
                json_free(member);
                json_free(obj);
                return NULL;
            }
            obj->keys = keys;
            obj->members = members;
            obj->keys[obj->nmembers] = key->string;
            key->string = NULL;
            json_free(key);
            obj->members[obj->nmembers] = member;
            obj->nmembers++;
            p = skip_ws(*cursor, end);
            if (p < end && *p == ',') { p++; p = skip_ws(p, end); continue; }
            if (p < end && *p == '}') { *cursor = p + 1; return obj; }
            json_free(obj);
            return NULL;
        }
    }
    if (*p == '[') {
        json_value *arr = json_new(JSON_ARRAY);
        if (!arr) return NULL;
        p++;
        p = skip_ws(p, end);
        if (p < end && *p == ']') { *cursor = p + 1; return arr; }
        for (;;) {
            *cursor = p;
            json_value *item = parse_value(cursor, end, depth + 1);
            if (!item) { json_free(arr); return NULL; }
            json_value **items = realloc(arr->items, (arr->nitems + 1) * sizeof *items);
            if (!items) { json_free(item); json_free(arr); return NULL; }
            arr->items = items;
            arr->items[arr->nitems++] = item;
            p = skip_ws(*cursor, end);
            if (p < end && *p == ',') { p++; p = skip_ws(p, end); continue; }
            if (p < end && *p == ']') { *cursor = p + 1; return arr; }
            json_free(arr);
            return NULL;
        }
    }
    if (p + 4 <= end && strncmp(p, "true", 4) == 0) {
        json_value *value = json_new(JSON_BOOL);
        if (!value) return NULL;
        value->bool_value = 1;
        *cursor = p + 4;
        return value;
    }
    if (p + 5 <= end && strncmp(p, "false", 5) == 0) {
        json_value *value = json_new(JSON_BOOL);
        if (!value) return NULL;
        *cursor = p + 5;
        return value;
    }
    if (p + 4 <= end && strncmp(p, "null", 4) == 0) {
        json_value *value = json_new(JSON_NULL);
        if (!value) return NULL;
        *cursor = p + 4;
        return value;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        const char *start = p++;
        while (p < end && strchr("0123456789+-.eE", *p)) p++;
        json_value *value = json_new(JSON_NUMBER);
        if (!value) return NULL;
        value->string = strndup(start, (size_t)(p - start));
        *cursor = p;
        return value;
    }
    return NULL;
}

static json_value *json_parse(const unsigned char *data, size_t len) {
    const char *cursor = (const char *)data;
    const char *end = cursor + len;
    json_value *value = parse_value(&cursor, end, 0);
    cursor = skip_ws(cursor, end);
    if (!value || cursor != end) { json_free(value); return NULL; }
    return value;
}

static json_value *json_get(const json_value *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->nmembers; i++) {
        if (strcmp(obj->keys[i], key) == 0) return obj->members[i];
    }
    return NULL;
}

static int append_bytes(char **buf, size_t *len, size_t *cap, const void *data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t next = *cap ? *cap * 2 : 64;
        while (next < *len + n + 1) next *= 2;
        char *grown = realloc(*buf, next);
        if (!grown) return -1;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

static int append_str(char **buf, size_t *len, size_t *cap, const char *text) {
    return append_bytes(buf, len, cap, text, strlen(text));
}

static int append_json_string(char **buf, size_t *len, size_t *cap, const char *text) {
    if (append_str(buf, len, cap, "\"") != 0) return -1;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        char esc[8];
        if (*p == '"' || *p == '\\') snprintf(esc, sizeof esc, "\\%c", *p);
        else if (*p == '\n') snprintf(esc, sizeof esc, "\\n");
        else if (*p == '\r') snprintf(esc, sizeof esc, "\\r");
        else if (*p == '\t') snprintf(esc, sizeof esc, "\\t");
        else if (*p < 32) snprintf(esc, sizeof esc, "\\u%04x", *p);
        else { esc[0] = (char)*p; esc[1] = 0; }
        if (append_str(buf, len, cap, esc) != 0) return -1;
    }
    return append_str(buf, len, cap, "\"");
}

static int query_append(char **url, const char *key, const char *value) {
    size_t len = strlen(*url);
    size_t cap = len + 1;
    char *buf = *url;
    char sep = strchr(buf, '?') ? '&' : '?';
    char piece[8];
    piece[0] = sep;
    piece[1] = 0;
    if (append_str(&buf, &len, &cap, piece) || append_str(&buf, &len, &cap, key) || append_str(&buf, &len, &cap, "=")) {
        *url = buf;
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        if (isalnum(*p) || strchr("-_.~", *p)) {
            char c = (char)*p;
            if (append_bytes(&buf, &len, &cap, &c, 1)) { *url = buf; return -1; }
        } else if (*p == ' ') {
            if (append_str(&buf, &len, &cap, "+")) { *url = buf; return -1; }
        } else {
            char enc[8];
            snprintf(enc, sizeof enc, "%%%02X", *p);
            if (append_str(&buf, &len, &cap, enc)) { *url = buf; return -1; }
        }
    }
    *url = buf;
    return 0;
}

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(unsigned char *p, uint16_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void wr32(unsigned char *p, uint32_t v) {
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255;
}

static int wav_parse(const unsigned char *data, size_t len, int *channels, int *width, int *rate,
    unsigned char **frames, size_t *frame_len, int streaming_wav) {
    *frames = NULL;
    *frame_len = 0;
    if (len < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return -1;
    int got_fmt = 0, ch = 0, bits = 0, sample_rate = 0;
    const unsigned char *payload = NULL;
    size_t declared = 0, available = 0;
    size_t off = 12;
    while (off + 8 <= len) {
        const unsigned char *id = data + off;
        uint32_t size = rd32(data + off + 4);
        off += 8;
        size_t have = off < len ? len - off : 0;
        if (memcmp(id, "fmt ", 4) == 0 && have >= 16 && size >= 16) {
            if (rd16(data + off) != 1) return -1;
            ch = rd16(data + off + 2);
            sample_rate = (int)rd32(data + off + 4);
            bits = rd16(data + off + 14);
            got_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            /* Configured streaming endpoints can use an unknown-length marker. */
            int streaming = streaming_wav && size == UINT32_C(0x7fff0000);
            if (size > have && !streaming) return -1;
            declared = size;
            payload = data + off;
            available = streaming ? have : size;
        }
        if (off + size > len) break;
        off += size + (size & 1);
    }
    if (!got_fmt || !payload || ch <= 0 || bits <= 0 || bits % 8 || sample_rate <= 0) return -1;
    *channels = ch;
    *width = bits / 8;
    *rate = sample_rate;
    *frame_len = available;
    *frames = malloc(available ? available : 1);
    if (!*frames) return -1;
    if (available) memcpy(*frames, payload, available);
    (void)declared;
    return 0;
}

static int wav_header(unsigned char hdr[44], int channels, int width, int rate, uint32_t nbytes) {
    memset(hdr, 0, 44);
    memcpy(hdr, "RIFF", 4);
    wr32(hdr + 4, 36 + nbytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wr32(hdr + 16, 16);
    wr16(hdr + 20, 1);
    wr16(hdr + 22, (uint16_t)channels);
    wr32(hdr + 24, (uint32_t)rate);
    wr32(hdr + 28, (uint32_t)(rate * channels * width));
    wr16(hdr + 32, (uint16_t)(channels * width));
    wr16(hdr + 34, (uint16_t)(width * 8));
    memcpy(hdr + 36, "data", 4);
    wr32(hdr + 40, nbytes);
    return 0;
}

static int unnamed_fd(const char *runtime) {
    char tmpl[PATH_MAX];
    if (snprintf(tmpl, sizeof tmpl, "%s/speechXXXXXX", runtime && runtime[0] ? runtime : "/tmp") >= (int)sizeof tmpl)
        return -1;
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    return fd;
}

int audio_speech_chunks(const char *text, char ***out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!text || !text[0]) return 0;
    size_t bytes = strlen(text);
    if (bytes > 1024 * 1024) return -1;
    size_t cap = bytes + 1;
    char **chunks = calloc(cap, sizeof *chunks);
    if (!chunks) return -1;
    size_t n = 0;
    if (speech_chunks(text, chunks, &n, cap) != 0) {
        for (size_t i = 0; i < n; i++) free(chunks[i]);
        free(chunks);
        return -1;
    }
    *out = chunks;
    *count = n;
    return 0;
}

static void free_chunks(char **chunks, size_t count) {
    if (!chunks) return;
    for (size_t i = 0; i < count; i++) free(chunks[i]);
    free(chunks);
}

typedef struct curl_buf {
    unsigned char *data;
    size_t len;
    size_t max;
    int overflow;
} curl_buf;

static size_t curl_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    curl_buf *buf = userdata;
    size_t n = size * nmemb;
    if (buf->len >= buf->max) { buf->overflow = 1; return n; }
    size_t room = buf->max - buf->len;
    size_t take = n < room ? n : room;
    memcpy(buf->data + buf->len, ptr, take);
    buf->len += take;
    if (take < n) buf->overflow = 1;
    return n;
}

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static void curl_init_once(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static int http_curl(const audio_http_request *request, audio_http_response *response) {
    pthread_once(&curl_once, curl_init_once);
    CURL *curl = curl_easy_init();
    if (!curl) { response->transport_error = 1; return 0; }
    size_t limit = request->maximum ? request->maximum : (size_t)LISTEN_MAX;
    if (limit > SIZE_MAX - 1) limit = SIZE_MAX - 1;
    curl_buf buf = {.max = limit + 1};
    buf.data = malloc(buf.max ? buf.max : 1);
    if (!buf.data) { curl_easy_cleanup(curl); response->transport_error = 1; return 0; }
    struct curl_slist *headers = NULL;
    if (request->content_type) {
        char *line = malloc(strlen(request->content_type) + 16);
        if (line) {
            sprintf(line, "Content-Type: %s", request->content_type);
            headers = curl_slist_append(headers, line);
            free(line);
        }
    }
    if (request->authorization) {
        char *line = malloc(strlen(request->authorization) + 24);
        if (line) {
            sprintf(line, "Authorization: %s", request->authorization);
            headers = curl_slist_append(headers, line);
            free(line);
        }
    }
    if (request->context_words) {
        char line[96];
        snprintf(line, sizeof line, "X-Voice-Context-Words: %zu", request->context_words);
        headers = curl_slist_append(headers, line);
    }
    curl_easy_setopt(curl, CURLOPT_URL, request->url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(request->timeout_ms > 0 ? request->timeout_ms : 90000));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (request->method && strcmp(request->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body ? (const char *)request->body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request->body_len);
    }
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    response->status = (int)status;
    response->body = buf.data;
    response->body_len = buf.overflow ? buf.max : buf.len;
    response->transport_error = rc != CURLE_OK;
    return 0;
}

static int http_call(audio *audio, const audio_http_request *request, audio_http_response *response) {
    memset(response, 0, sizeof *response);
    if (audio->http) return audio->http(request, response, audio->http_user);
    return http_curl(request, response);
}

static int abandoned(audio *audio, atomic_int *cancelled, int stop_gen) {
    if (cancelled && atomic_load(cancelled)) return 1;
    return atomic_load(&audio->stop_gen) != stop_gen;
}

/* The caller owns the cancel token and may free it after abandoning the job. */
static int job_stopped(job *job) {
    pthread_mutex_lock(&job->mu);
    int left = job->left;
    atomic_int *cancelled = left ? NULL : job->cancelled;
    int cancel = cancelled && atomic_load(cancelled);
    int generation = job->stop_gen;
    pthread_mutex_unlock(&job->mu);
    if (left || cancel) return 1;
    return atomic_load(&job->audio->stop_gen) != generation;
}

static void live_done(audio *audio) {
    pthread_mutex_lock(&audio->live_mu);
    audio->live--;
    pthread_cond_broadcast(&audio->live_cv);
    pthread_mutex_unlock(&audio->live_mu);
}

static int spawn_detached(audio *audio, void *(*fn)(void *), void *arg) {
    pthread_mutex_lock(&audio->live_mu);
    audio->live++;
    pthread_mutex_unlock(&audio->live_mu);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) { live_done(audio); return -1; }
    return 0;
}

static void free_result(op_result *result) {
    free(result->text);
    free(result->frames);
    result->text = NULL;
    result->frames = NULL;
}

static int lock_timeout(pthread_mutex_t *mu, int timeout_ms) {
    int waited = 0;
    for (;;) {
        int rc = pthread_mutex_trylock(mu);
        if (rc == 0) return 0;
        if (rc != EBUSY) return rc;
        if (waited >= timeout_ms) return ETIMEDOUT;
        sleep_ms(5);
        waited += 5;
    }
}

static void *job_main(void *arg) {
    job *job = arg;
    audio *owner = job->audio;
    int got_gate = 0;
    while (!job_stopped(job)) {
        int rc = lock_timeout(job->gate, 50);
        if (rc == 0) { got_gate = 1; break; }
        if (rc != ETIMEDOUT) {
            set_err(job->result.error, sizeof job->result.error, "Could not acquire the speech lock");
            job->result.status = -1;
            goto publish;
        }
    }
    if (!got_gate) goto publish;
    if (!job_stopped(job)) {
        int rc = job->op(job->user, &job->result);
        if (rc != 0) job->result.status = -1;
    }
    pthread_mutex_unlock(job->gate);
    got_gate = 0;
publish:
    if (got_gate) pthread_mutex_unlock(job->gate);
    if (job->free_user) job->free_user(job->user);
    job->user = NULL;
    if (job->result.status == 0 && job->on_drained && job->result.text)
        job->on_drained(job->result.text, job->drain_user);
    pthread_mutex_lock(&job->mu);
    if (job->left) {
        pthread_mutex_unlock(&job->mu);
        destroy_job_ctx(job);
        free_result(&job->result);
        pthread_mutex_destroy(&job->mu);
        pthread_cond_destroy(&job->cv);
        free(job);
        live_done(owner);
        return NULL;
    }
    job->done = 1;
    pthread_cond_signal(&job->cv);
    pthread_mutex_unlock(&job->mu);
    live_done(owner);
    return NULL;
}

static int run_cancellable(audio *audio, pthread_mutex_t *gate, atomic_int *cancelled,
    int (*op)(void *user, op_result *result), void *user, void (*free_user)(void *user),
    audio_drained_fn on_drained, void *drain_user, void (*destroy_ctx)(void *), void *destroy_arg,
    op_result *result, char *err, size_t err_cap) {
    job *job = calloc(1, sizeof *job);
    if (!job) {
        if (free_user) free_user(user);
        if (destroy_ctx) destroy_ctx(destroy_arg);
        set_err(err, err_cap, "Out of memory");
        return -1;
    }
    job->audio = audio;
    job->gate = gate;
    job->cancelled = cancelled;
    job->stop_gen = atomic_load(&audio->stop_gen);
    job->op = op;
    job->user = user;
    job->free_user = free_user;
    job->on_drained = on_drained;
    job->drain_user = drain_user;
    job->destroy_ctx = destroy_ctx;
    job->destroy_arg = destroy_arg;
    pthread_mutex_init(&job->mu, NULL);
    pthread_cond_init(&job->cv, NULL);
    if (spawn_detached(audio, job_main, job) != 0) {
        if (free_user) free_user(user);
        destroy_job_ctx(job);
        pthread_mutex_destroy(&job->mu);
        pthread_cond_destroy(&job->cv);
        free(job);
        set_err(err, err_cap, "Could not start the speech worker");
        return -1;
    }
    pthread_mutex_lock(&job->mu);
    for (;;) {
        if (job->done) break;
        if (abandoned(audio, cancelled, job->stop_gen)) {
            job->cancelled = NULL;
            job->left = 1;
            pthread_mutex_unlock(&job->mu);
            return 1;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 25 * 1000 * 1000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&job->cv, &job->mu, &ts);
    }
    int was_abandoned = abandoned(audio, cancelled, job->stop_gen);
    if (!was_abandoned && result) *result = job->result;
    else free_result(&job->result);
    int status = job->result.status;
    char error_copy[512];
    snprintf(error_copy, sizeof error_copy, "%s", job->result.error);
    pthread_mutex_unlock(&job->mu);
    destroy_job_ctx(job);
    pthread_mutex_destroy(&job->mu);
    pthread_cond_destroy(&job->cv);
    free(job);
    if (was_abandoned) return 1;
    if (status != 0) { set_err(err, err_cap, error_copy); return -1; }
    return 0;
}

void audio_config_init(audio_config *config) {
    memset(config, 0, sizeof *config);
}

static audio_pair *copy_pairs(const audio_pair *pairs, size_t count) {
    if (!count) return NULL;
    audio_pair *out = calloc(count, sizeof *out);
    if (!out) return NULL;
    for (size_t i = 0; i < count; i++) {
        out[i].key = strdup(pairs[i].key ? pairs[i].key : "");
        out[i].value = strdup(pairs[i].value ? pairs[i].value : "");
        if (!out[i].key || !out[i].value) {
            for (size_t j = 0; j <= i; j++) {
                free((void *)out[j].key);
                free((void *)out[j].value);
            }
            free(out);
            return NULL;
        }
    }
    return out;
}

static void free_pairs(audio_pair *pairs, size_t count) {
    if (!pairs) return;
    for (size_t i = 0; i < count; i++) {
        free((char *)pairs[i].key);
        free((char *)pairs[i].value);
    }
    free(pairs);
}

audio *audio_new(const char *runtime_dir, const audio_config *config) {
    audio_config defaults;
    if (!config) {
        audio_config_init(&defaults);
        config = &defaults;
    }
    audio *audio = calloc(1, sizeof *audio);
    if (!audio) return NULL;
    audio->runtime = strdup(runtime_dir ? runtime_dir : "/tmp");
    audio->stt_prompt = dup_opt(config->stt_prompt);
    audio->stt_language = strdup(config->stt_language ? config->stt_language : "en");
    audio->preferred_microphone = dup_opt(config->preferred_microphone);
    audio->playback_mode = strdup(config->playback_mode ? config->playback_mode : "buffered");
    audio->speech_preferences = sibling_name(config->voice_preferences_path, "speech-backend");
    audio->stt_preferences = config->stt_preferences_path ? strdup(config->stt_preferences_path)
        : sibling_name(config->voice_preferences_path, "stt-backend");
    snprintf(audio->stt_state, sizeof audio->stt_state, "unknown");
    snprintf(audio->tts_state, sizeof audio->tts_state, "unknown");
    pthread_mutex_init(&audio->mu, NULL);
    pthread_mutex_init(&audio->backend_mu, NULL);
    pthread_mutex_init(&audio->synthesis_mu, NULL);
    pthread_mutex_init(&audio->recognition_mu, NULL);
    pthread_mutex_init(&audio->call_mu, NULL);
    pthread_mutex_init(&audio->live_mu, NULL);
    pthread_cond_init(&audio->live_cv, NULL);
    if (config->backend_count > 4 || (config->backend_count && !config->backends)) goto invalid;
    for (size_t i = 0; i < config->backend_count; i++) {
        const audio_backend *source = &config->backends[i];
        audio_backend *target = &audio->backends[i];
        audio->backend_count++;
        if (!source->id || !source->id[0] || strlen(source->id) > 31
            || (!source->listen_url && !source->speak_url)
            || source->voice_count > 48 || source->query_count > 16
            || (source->voice_count && !source->voices) || (source->query_count && !source->query)
            || (source->speak_url && !source->voice_count)) goto invalid;
        target->id = strdup(source->id);
        target->label = strdup(source->label ? source->label : source->id);
        target->listen_url = dup_opt(source->listen_url);
        target->speak_url = dup_opt(source->speak_url);
        target->auth_env = dup_opt(source->auth_env);
        target->auth_scheme = strdup(source->auth_scheme ? source->auth_scheme : "Token");
        target->listen_model = dup_opt(source->listen_model);
        target->default_voice = dup_opt(source->default_voice);
        target->voice_preferences_path = dup_opt(source->voice_preferences_path);
        target->timeout_ms = source->timeout_ms > 0 ? source->timeout_ms : 180000;
        target->streaming_wav = source->streaming_wav;
        target->query = copy_pairs(source->query, source->query_count);
        target->query_count = source->query_count;
        audio_voice *voices = calloc(source->voice_count ? source->voice_count : 1, sizeof *voices);
        target->voices = voices;
        if (!target->id || !target->label || !target->auth_scheme || !voices
            || (source->listen_url && !target->listen_url) || (source->speak_url && !target->speak_url)
            || (source->auth_env && !target->auth_env) || (source->listen_model && !target->listen_model)
            || (source->default_voice && !target->default_voice)
            || (source->voice_preferences_path && !target->voice_preferences_path)
            || (source->query_count && !target->query)) goto invalid;
        for (size_t v = 0; v < source->voice_count; v++) {
            if (!source->voices[v].id || !source->voices[v].id[0] || strlen(source->voices[v].id) > 31
                || !source->voices[v].model || !source->voices[v].model[0] || strlen(source->voices[v].model) > 127) goto invalid;
            voices[v].id = strdup(source->voices[v].id);
            voices[v].label = strdup(source->voices[v].label ? source->voices[v].label : source->voices[v].id);
            voices[v].model = strdup(source->voices[v].model);
            target->voice_count++;
            if (!voices[v].id || !voices[v].label || !voices[v].model) goto invalid;
        }
        if (target->voice_count) {
            const char *initial = voice_index(target, target->default_voice) >= 0 ? target->default_voice : voices[0].id;
            snprintf(audio->selected_voices[i], sizeof audio->selected_voices[i], "%s", initial);
            char *saved = read_pref(target->voice_preferences_path);
            if (voice_index(target, saved) >= 0)
                snprintf(audio->selected_voices[i], sizeof audio->selected_voices[i], "%s", saved);
            free(saved);
        }
        if (backend_available(target)) {
            if (!audio->speech_backend[0] && target->speak_url)
                snprintf(audio->speech_backend, sizeof audio->speech_backend, "%s", target->id);
            if (!audio->stt_backend[0] && target->listen_url)
                snprintf(audio->stt_backend, sizeof audio->stt_backend, "%s", target->id);
        }
    }
    char *saved = read_pref(audio->speech_preferences);
    if (backend_index(audio, saved, 1) >= 0)
        snprintf(audio->speech_backend, sizeof audio->speech_backend, "%s", saved);
    free(saved);
    saved = read_pref(audio->stt_preferences);
    if (backend_index(audio, saved, 0) >= 0)
        snprintf(audio->stt_backend, sizeof audio->stt_backend, "%s", saved);
    free(saved);
    if (config->stt_backend) {
        if (backend_index(audio, config->stt_backend, 0) < 0) goto invalid;
        snprintf(audio->stt_backend, sizeof audio->stt_backend, "%s", config->stt_backend);
    }
    if (config->speech_backend) {
        if (backend_index(audio, config->speech_backend, 1) < 0) goto invalid;
        snprintf(audio->speech_backend, sizeof audio->speech_backend, "%s", config->speech_backend);
    }
    return audio;
invalid:
    audio_free(audio);
    return NULL;
}

void audio_free(audio *audio) {
    if (!audio) return;
    pthread_mutex_lock(&audio->live_mu);
    while (audio->live) pthread_cond_wait(&audio->live_cv, &audio->live_mu);
    pthread_mutex_unlock(&audio->live_mu);
    if (audio->player && audio->player->poll && audio->player->poll(audio->player) == -1 && audio->player->terminate)
        audio->player->terminate(audio->player);
    if (audio->player && audio->player->destroy) audio->player->destroy(audio->player);
    free(audio->runtime);
    free(audio->stt_prompt);
    free(audio->stt_language);
    free(audio->preferred_microphone);
    free(audio->speech_preferences);
    free(audio->stt_preferences);
    free(audio->playback_mode);
    for (size_t i = 0; i < audio->backend_count; i++) {
        audio_backend *b = &audio->backends[i];
        free((void *)b->id);
        free((void *)b->label);
        free((void *)b->listen_url);
        free((void *)b->speak_url);
        free((void *)b->auth_env);
        free((void *)b->auth_scheme);
        free((void *)b->listen_model);
        free((void *)b->default_voice);
        free((void *)b->voice_preferences_path);
        free_pairs((audio_pair *)b->query, b->query_count);
        for (size_t v = 0; v < b->voice_count; v++) {
            free((void *)b->voices[v].id);
            free((void *)b->voices[v].label);
            free((void *)b->voices[v].model);
        }
        free((void *)b->voices);
    }
    pthread_mutex_destroy(&audio->mu);
    pthread_mutex_destroy(&audio->backend_mu);
    pthread_mutex_destroy(&audio->synthesis_mu);
    pthread_mutex_destroy(&audio->recognition_mu);
    pthread_mutex_destroy(&audio->call_mu);
    pthread_mutex_destroy(&audio->live_mu);
    pthread_cond_destroy(&audio->live_cv);
    free(audio);
}

void audio_set_http(audio *audio, audio_http_fn fn, void *user) { audio->http = fn; audio->http_user = user; }
void audio_set_popen(audio *audio, audio_popen_fn fn, void *user) { audio->popen_fn = fn; audio->popen_user = user; }
void audio_set_run(audio *audio, audio_run_fn fn, void *user) { audio->run_fn = fn; audio->run_user = user; }
void audio_set_audible(audio *audio, audio_audible_fn fn, void *user) { audio->audible = fn; audio->audible_user = user; }
void audio_set_microphone(audio *audio, audio_mic_status_fn status, audio_mic_resolve_fn resolve, void *user) {
    audio->mic_status = status; audio->mic_resolve = resolve; audio->mic_user = user;
}
void audio_set_microphone_details(audio *audio, audio_mic_details_fn fn, void *user) {
    audio->mic_details = fn;
    audio->mic_details_user = user;
}
const char *audio_preferred_microphone(const audio *audio) {
    return audio && audio->preferred_microphone ? audio->preferred_microphone : NULL;
}
void audio_set_capture(audio *audio, audio_capture_fn fn, void *user) { audio->capture = fn; audio->capture_user = user; }
void audio_set_playback_mode(audio *audio, const char *mode) {
    pthread_mutex_lock(&audio->mu);
    free(audio->playback_mode);
    audio->playback_mode = strdup(mode && mode[0] ? mode : "buffered");
    pthread_mutex_unlock(&audio->mu);
}

static int select_backend(audio *audio, const char *id, int speech, char *err, size_t cap) {
    int index = backend_index(audio, id, speech);
    if (index < 0 || !backend_available(&audio->backends[index])) {
        set_err(err, cap, "That backend is unavailable or its credential is missing");
        return -1;
    }
    pthread_mutex_lock(&audio->mu);
    if (save_pref(speech ? audio->speech_preferences : audio->stt_preferences, id) != 0) {
        pthread_mutex_unlock(&audio->mu);
        set_err(err, cap, "Could not save the backend choice");
        return -1;
    }
    snprintf(speech ? audio->speech_backend : audio->stt_backend, 32, "%s", id);
    pthread_mutex_unlock(&audio->mu);
    set_backend(audio, speech ? "tts" : "stt", "unknown", NULL);
    return 0;
}

int audio_set_speech_backend(audio *audio, const char *backend, char *err, size_t err_cap) {
    return select_backend(audio, backend, 1, err, err_cap);
}

int audio_set_stt_backend(audio *audio, const char *backend, char *err, size_t err_cap) {
    return select_backend(audio, backend, 0, err, err_cap);
}

int audio_set_voice(audio *audio, const char *character, char *err, size_t err_cap) {
    pthread_mutex_lock(&audio->mu);
    int index = backend_index(audio, audio->speech_backend, 1);
    if (index < 0 || voice_index(&audio->backends[index], character) < 0) {
        set_err(err, err_cap, "Unknown voice for the selected backend");
        pthread_mutex_unlock(&audio->mu);
        return -1;
    }
    if (save_pref(audio->backends[index].voice_preferences_path, character) != 0) {
        set_err(err, err_cap, "Could not save the voice");
        pthread_mutex_unlock(&audio->mu);
        return -1;
    }
    snprintf(audio->selected_voices[index], sizeof audio->selected_voices[index], "%s", character);
    pthread_mutex_unlock(&audio->mu);
    return 0;
}

int audio_status(audio *audio, audio_report *status) {
    memset(status, 0, sizeof *status);
    pthread_mutex_lock(&audio->mu);
    snprintf(status->speech_backend, sizeof status->speech_backend, "%s", audio->speech_backend);
    snprintf(status->selected_stt, sizeof status->selected_stt, "%s", audio->stt_backend);
    int selected = backend_index(audio, audio->speech_backend, 1);
    if (selected >= 0) {
        const audio_backend *backend = &audio->backends[selected];
        snprintf(status->selected_voice, sizeof status->selected_voice, "%s", audio->selected_voices[selected]);
        status->voice_count = backend->voice_count;
        for (size_t i = 0; i < backend->voice_count; i++) {
            snprintf(status->voice_ids[i], sizeof status->voice_ids[i], "%s", backend->voices[i].id);
            snprintf(status->voice_labels[i], sizeof status->voice_labels[i], "%s", backend->voices[i].label);
        }
    }
    for (size_t i = 0; i < audio->backend_count; i++) {
        const audio_backend *backend = &audio->backends[i];
        if (!backend_available(backend)) continue;
        if (backend->speak_url) {
            size_t n = status->speech_backend_count++;
            snprintf(status->speech_backend_ids[n], 32, "%s", backend->id);
            snprintf(status->speech_backend_labels[n], 64, "%s", backend->label);
        }
        if (backend->listen_url) {
            size_t n = status->stt_count++;
            snprintf(status->stt_ids[n], 32, "%s", backend->id);
            snprintf(status->stt_labels[n], 64, "%s", backend->label);
        }
    }
    int hide_tts = selected < 0;
    pthread_mutex_unlock(&audio->mu);
    pthread_mutex_lock(&audio->backend_mu);
    status->has_stt = 1;
    snprintf(status->stt_state, sizeof status->stt_state, "%s", audio->stt_state);
    status->has_stt_error = audio->has_stt_error;
    if (audio->has_stt_error) snprintf(status->stt_error, sizeof status->stt_error, "%s", audio->stt_error);
    if (!hide_tts) {
        status->has_tts = 1;
        snprintf(status->tts_state, sizeof status->tts_state, "%s", audio->tts_state);
        status->has_tts_error = audio->has_tts_error;
        if (audio->has_tts_error) snprintf(status->tts_error, sizeof status->tts_error, "%s", audio->tts_error);
    }
    pthread_mutex_unlock(&audio->backend_mu);
    if (audio->mic_details) {
        audio_mic_details details;
        memset(&details, 0, sizeof details);
        audio->mic_details(audio->mic_details_user, &details);
        snprintf(status->microphone_name, sizeof status->microphone_name, "%s", details.name);
        snprintf(status->microphone_target, sizeof status->microphone_target, "%s", details.has_target ? details.target : "");
        status->microphone_has_target = details.has_target;
        status->microphone_muted = details.muted;
        status->microphone_muted_known = details.muted_known;
        snprintf(status->microphone_preferred, sizeof status->microphone_preferred, "%s", details.has_preferred ? details.preferred : "");
        status->microphone_has_preferred = details.has_preferred;
        status->microphone_missing = details.missing;
        snprintf(status->microphone_error, sizeof status->microphone_error, "%s", details.has_error ? details.error : "");
        status->microphone_has_error = details.has_error;
    } else if (audio->mic_status) {
        audio->mic_status(audio->mic_user, status->microphone_name, sizeof status->microphone_name,
            status->microphone_target, sizeof status->microphone_target);
        status->microphone_has_target = status->microphone_target[0] != '\0';
    }
    return 0;
}

int audio_microphone_json(audio *audio, char **json) {
    if (json) *json = NULL;
    if (!audio || !json) return -1;
    audio_report report;
    if (audio_status(audio, &report) != 0) return -1;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return -1;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strncpy(doc, root, "name", report.microphone_name, strlen(report.microphone_name));
    if (report.microphone_has_target)
        yyjson_mut_obj_add_strncpy(doc, root, "target", report.microphone_target, strlen(report.microphone_target));
    else yyjson_mut_obj_add_null(doc, root, "target");
    if (report.microphone_muted_known) yyjson_mut_obj_add_bool(doc, root, "muted", report.microphone_muted);
    else yyjson_mut_obj_add_null(doc, root, "muted");
    if (report.microphone_has_preferred)
        yyjson_mut_obj_add_strncpy(doc, root, "preferred", report.microphone_preferred, strlen(report.microphone_preferred));
    else yyjson_mut_obj_add_null(doc, root, "preferred");
    yyjson_mut_obj_add_bool(doc, root, "missing", report.microphone_missing);
    if (report.microphone_has_error)
        yyjson_mut_obj_add_strncpy(doc, root, "error", report.microphone_error, strlen(report.microphone_error));
    else yyjson_mut_obj_add_null(doc, root, "error");
    *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return *json ? 0 : -1;
}

int audio_playing(audio *audio) {
    pthread_mutex_lock(&audio->mu);
    int playing = audio->player != NULL;
    pthread_mutex_unlock(&audio->mu);
    return playing;
}

static int parse_wav_result(const unsigned char *data, size_t len, int streaming_wav, op_result *result) {
    int channels = 0, width = 0, rate = 0;
    unsigned char *frames = NULL;
    size_t frame_len = 0;
    if (wav_parse(data, len, &channels, &width, &rate, &frames, &frame_len, streaming_wav) != 0 || !frames || frame_len == 0
        || width <= 0 || channels <= 0 || frame_len % (size_t)(channels * width) != 0) {
        free(frames);
        set_err(result->error, sizeof result->error, "Incomplete speech audio");
        return -1;
    }
    if (!streaming_wav) {
        size_t declared = 0;
        if (len >= 44 && memcmp(data + 36, "data", 4) == 0) declared = rd32(data + 40);
        else declared = frame_len;
        if (frame_len != declared) {
            free(frames);
            set_err(result->error, sizeof result->error, "Incomplete speech audio");
            return -1;
        }
    }
    result->frames = frames;
    result->frame_len = frame_len;
    result->channels = channels;
    result->width = width;
    result->rate = rate;
    return 0;
}

typedef struct http_op {
    audio *audio;
    char *url;
    char *content_type;
    char *authorization;
    unsigned char *body;
    size_t body_len;
    int timeout_ms;
    size_t maximum;
    int listen;
    size_t context_words;
    const audio_backend *backend;
} http_op;

static void free_http_op(void *user) {
    http_op *op = user;
    if (!op) return;
    free(op->url);
    free(op->content_type);
    free(op->authorization);
    free(op->body);
    free(op);
}

static int http_op_run_inner(void *user, op_result *result) {
    http_op *op = user;
    audio_http_request request = {
        .method = "POST", .url = op->url, .content_type = op->content_type,
        .authorization = op->authorization, .body = op->body, .body_len = op->body_len,
        .timeout_ms = op->timeout_ms, .maximum = op->maximum, .context_words = op->context_words
    };
    audio_http_response response;
    http_call(op->audio, &request, &response);
    if (response.verbatim_error[0]) {
        set_err(result->error, sizeof result->error, response.verbatim_error);
        free(response.body);
        return -1;
    }
    if (response.transport_error || response.status == 0) {
        snprintf(result->error, sizeof result->error, "%s connection failed", op->backend->label);
        free(response.body);
        return -1;
    }
    if (response.status < 200 || response.status >= 300) {
        snprintf(result->error, sizeof result->error, "%s request failed (HTTP %d)", op->backend->label, response.status);
        free(response.body);
        return -1;
    }
    if (response.body_len > op->maximum) {
        set_err(result->error, sizeof result->error, "Backend response exceeds the size limit");
        free(response.body);
        return -1;
    }
    if (op->listen) {
        json_value *root = json_parse(response.body, response.body_len);
        free(response.body);
        json_value *results = root ? json_get(root, "results") : NULL;
        json_value *channels = results ? json_get(results, "channels") : NULL;
        json_value *text = NULL;
        if (channels && channels->type == JSON_ARRAY && channels->nitems) {
            json_value *alternatives = json_get(channels->items[0], "alternatives");
            if (alternatives && alternatives->type == JSON_ARRAY && alternatives->nitems)
                text = json_get(alternatives->items[0], "transcript");
        }
        if (text && text->type == JSON_STRING) result->text = strdup(text->string);
        json_free(root);
        if (!result->text) {
            snprintf(result->error, sizeof result->error, "%s returned invalid JSON transcription", op->backend->label);
            return -1;
        }
        return 0;
    }
    int rc = parse_wav_result(response.body, response.body_len, op->backend->streaming_wav, result);
    free(response.body);
    return rc;
}

static int http_op_run(void *user, op_result *result) {
    http_op *op = user;
    int rc = http_op_run_inner(user, result);
    set_backend(op->audio, op->listen ? "stt" : "tts", rc ? "error" : "ready", rc ? result->error : NULL);
    return rc;
}

static void destroy_unused(void (*destroy_ctx)(void *), void *drain_user) {
    if (destroy_ctx) destroy_ctx(drain_user);
}

static http_op *new_request(audio *audio, const audio_backend *backend, int listen, char *err, size_t cap) {
    if (!backend || !backend_available(backend)) {
        set_err(err, cap, "Backend is unavailable or its credential is missing");
        return NULL;
    }
    http_op *op = calloc(1, sizeof *op);
    if (!op) return NULL;
    op->audio = audio;
    op->backend = backend;
    op->listen = listen;
    op->url = dup_opt(listen ? backend->listen_url : backend->speak_url);
    op->content_type = strdup(listen ? "audio/wav" : "application/json");
    op->timeout_ms = backend->timeout_ms;
    op->maximum = listen ? LISTEN_MAX : TTS_MAX;
    if (!op->url || !op->content_type) goto fail;
    if (backend->auth_env) {
        const char *key = getenv(backend->auth_env);
        if (!key || strpbrk(key, "\r\n")) goto fail;
        if (asprintf(&op->authorization, "%s %s", backend->auth_scheme, key) < 0) goto fail;
    }
    for (size_t i = 0; i < backend->query_count; i++)
        if (query_append(&op->url, backend->query[i].key, backend->query[i].value)) goto fail;
    set_backend(audio, listen ? "stt" : "tts", "loading", NULL);
    return op;
fail:
    free_http_op(op);
    set_err(err, cap, "Could not construct the backend request");
    return NULL;
}

static int transcribe_request(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, void (*destroy_ctx)(void *),
    char *out, size_t out_cap, char *err, size_t err_cap) {
    pthread_mutex_lock(&audio->mu);
    int index = backend_index(audio, audio->stt_backend, 0);
    const audio_backend *backend = index >= 0 ? &audio->backends[index] : NULL;
    pthread_mutex_unlock(&audio->mu);
    http_op *op = new_request(audio, backend, 1, err, err_cap);
    if (!op) {
        destroy_unused(destroy_ctx, drain_user);
        return -1;
    }
    if (read_file(path, &op->body, &op->body_len) != 0) {
        free_http_op(op);
        destroy_unused(destroy_ctx, drain_user);
        set_err(err, err_cap, "Could not read dictation audio");
        return -1;
    }
    char *url = op->url;
    if (backend->listen_model) query_append(&url, "model", backend->listen_model);
    query_append(&url, "language", audio->stt_language);
    if (audio->stt_prompt) {
        char *copy = strdup(audio->stt_prompt);
        char *start = copy;
        for (char *p = copy; copy && *p; ) {
            if (*p == ',') {
                *p = 0;
                strip(start);
                if (start[0]) query_append(&url, "keyterm", start);
                start = p + 1;
                p++;
            } else p++;
        }
        if (copy) { strip(start); if (start[0]) query_append(&url, "keyterm", start); }
        free(copy);
    }
    op->url = url;
    op_result result = {0};
    int rc = run_cancellable(audio, &audio->recognition_mu, cancelled, http_op_run, op, free_http_op,
        on_drained, drain_user, destroy_ctx, drain_user, &result, err, err_cap);
    if (rc != 0) return rc < 0 ? -1 : 0;
    if (!out || strlen(result.text) + 1 > out_cap) {
        free_result(&result);
        set_err(err, err_cap, "transcription exceeds the buffer");
        return -1;
    }
    memcpy(out, result.text, strlen(result.text) + 1);
    free_result(&result);
    return 0;
}

static int transcribe_dispatch(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, void (*destroy_ctx)(void *),
    char *out, size_t out_cap, char *err, size_t err_cap) {
    if (out && out_cap) out[0] = 0;
    if (!audio || !path) {
        destroy_unused(destroy_ctx, drain_user);
        set_err(err, err_cap, "transcription is unavailable");
        return -1;
    }
    if (cancelled && atomic_load(cancelled)) {
        destroy_unused(destroy_ctx, drain_user);
        return 0;
    }
    return transcribe_request(audio, path, cancelled, on_drained, drain_user, destroy_ctx, out, out_cap, err, err_cap);
}

int audio_transcribe(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, char *out, size_t out_cap, char *err, size_t err_cap) {
    return transcribe_dispatch(audio, path, cancelled, on_drained, drain_user, NULL, out, out_cap, err, err_cap);
}

int audio_transcribe_owned(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, void (*destroy_ctx)(void *),
    char *out, size_t out_cap, char *err, size_t err_cap) {
    return transcribe_dispatch(audio, path, cancelled, on_drained, drain_user, destroy_ctx, out, out_cap, err, err_cap);
}

void audio_drain(audio *audio) {
    if (!audio) return;
    pthread_mutex_lock(&audio->live_mu);
    while (audio->live) pthread_cond_wait(&audio->live_cv, &audio->live_mu);
    pthread_mutex_unlock(&audio->live_mu);
}

static void free_choice(voice_choice *choice) { (void)choice; }

static int choose_voice(audio *audio, const char *text, voice_choice *choice, char *err, size_t err_cap) {
    memset(choice, 0, sizeof *choice);
    int in_word = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isspace(*p)) in_word = 0;
        else if (!in_word) { in_word = 1; choice->context_words++; }
    }
    pthread_mutex_lock(&audio->mu);
    int index = backend_index(audio, audio->speech_backend, 1);
    int voice = index >= 0 ? voice_index(&audio->backends[index], audio->selected_voices[index]) : -1;
    if (voice < 0) {
        pthread_mutex_unlock(&audio->mu);
        set_err(err, err_cap, "That speech backend or voice is not configured");
        return -1;
    }
    choice->backend = &audio->backends[index];
    snprintf(choice->model, sizeof choice->model, "%s", choice->backend->voices[voice].model);
    pthread_mutex_unlock(&audio->mu);
    return 0;
}

static int synthesize(audio *audio, const char *chunk, const voice_choice *choice, atomic_int *cancelled, op_result *result, char *err, size_t err_cap) {
    http_op *op = new_request(audio, choice->backend, 0, err, err_cap);
    if (!op) return -1;
    op->context_words = choice->context_words;
    char *text = NULL;
    size_t n = 0, cap = 0;
    if (query_append(&op->url, "model", choice->model)
        || query_append(&op->url, "encoding", "linear16")
        || query_append(&op->url, "container", "wav")
        || query_append(&op->url, "sample_rate", "24000")
        || append_str(&text, &n, &cap, "{\"text\":")
        || append_json_string(&text, &n, &cap, chunk)
        || append_str(&text, &n, &cap, "}")) {
        free(text);
        free_http_op(op);
        set_err(err, err_cap, "Could not construct the speech request");
        return -1;
    }
    op->body = (unsigned char *)text;
    op->body_len = n;
    return run_cancellable(audio, &audio->synthesis_mu, cancelled, http_op_run, op, free_http_op,
        NULL, NULL, NULL, NULL, result, err, err_cap);
}

typedef struct real_player {
    audio_player base;
    pid_t pid;
    int stdin_fd;
    int reaped;
    int code;
} real_player;

static real_player *real_of(audio_player *player) { return player->user; }

static int write_without_sigpipe(int fd, const void *data, size_t len) {
    sigset_t block, previous, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &block, &previous) != 0) return -1;
    int prior = 0;
    sigemptyset(&pending);
    if (sigpending(&pending) == 0) prior = sigismember(&pending, SIGPIPE);
    int rc = write_all(fd, data, len);
    if (!prior) {
        sigemptyset(&pending);
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
            struct timespec zero = {0, 0};
            while (sigtimedwait(&block, NULL, &zero) < 0 && errno == EINTR) {}
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return rc;
}

static int real_write(audio_player *player, const void *data, size_t len) {
    real_player *real = real_of(player);
    return write_without_sigpipe(real->stdin_fd, data, len) == 0 ? (int)len : -1;
}
static void real_close(audio_player *player) {
    real_player *real = real_of(player);
    if (real->stdin_fd >= 0) close(real->stdin_fd);
    real->stdin_fd = -1;
    player->stdin_closed = 1;
}
static int real_reap(real_player *real, int block) {
    if (real->reaped) return real->code;
    int status = 0;
    pid_t got = waitpid(real->pid, &status, block ? 0 : WNOHANG);
    if (got == 0) return -1;
    if (got < 0) { real->reaped = 1; real->code = 127; return real->code; }
    real->reaped = 1;
    real->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return real->code;
}
static int real_wait(audio_player *player, int timeout_ms) {
    real_player *real = real_of(player);
    if (timeout_ms < 0) return real_reap(real, 1);
    int waited = 0;
    while (waited <= timeout_ms) {
        int code = real_reap(real, 0);
        if (real->reaped) return code;
        sleep_ms(10);
        waited += 10;
    }
    return -1;
}
static int real_poll(audio_player *player) {
    real_player *real = real_of(player);
    if (real->reaped) return real->code;
    real_reap(real, 0);
    return real->reaped ? real->code : -1;
}
static void real_terminate(audio_player *player) {
    real_player *real = real_of(player);
    if (!real->reaped) kill(real->pid, SIGTERM);
}
static void real_kill(audio_player *player) {
    real_player *real = real_of(player);
    if (!real->reaped) kill(real->pid, SIGKILL);
}
static void real_destroy(audio_player *player) {
    real_player *real = real_of(player);
    real_close(player);
    if (real && !real->reaped && real->pid > 0) {
        kill(real->pid, SIGTERM);
        int waited = 0;
        while (waited < 200 && !real->reaped) {
            real_reap(real, 0);
            if (real->reaped) break;
            sleep_ms(10);
            waited += 10;
        }
        if (!real->reaped) {
            kill(real->pid, SIGKILL);
            real_reap(real, 1);
        }
    }
    free(real);
}

static audio_player *default_popen(const audio_spawn *spawn) {
    int pipefd[2] = {-1, -1};
    if (spawn->pipe_stdin && pipe2(pipefd, O_CLOEXEC) != 0) return NULL;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (spawn->pipe_stdin) {
        posix_spawn_file_actions_adddup2(&actions, pipefd[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    int rc = 0;
    if (spawn->pass_fd >= 0) {
        /* A spawn dup2 action clears CLOEXEC in the child even for fd == fd.
         * Never make the parent's private recording inheritable by other spawns. */
        rc = posix_spawn_file_actions_adddup2(&actions, spawn->pass_fd, spawn->pass_fd);
    }
    pid_t pid = 0;
    if (!rc) rc = posix_spawnp(&pid, spawn->argv[0], &actions, NULL, (char *const *)spawn->argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn->pipe_stdin) close(pipefd[0]);
    if (rc != 0) {
        if (spawn->pipe_stdin) close(pipefd[1]);
        return NULL;
    }
    real_player *real = calloc(1, sizeof *real);
    real->pid = pid;
    real->stdin_fd = spawn->pipe_stdin ? pipefd[1] : -1;
    real->base.write = real_write;
    real->base.close_stdin = real_close;
    real->base.wait = real_wait;
    real->base.poll = real_poll;
    real->base.terminate = real_terminate;
    real->base.kill = real_kill;
    real->base.destroy = real_destroy;
    real->base.user = real;
    return &real->base;
}

static audio_player *open_player(audio *audio, const audio_spawn *spawn) {
    if (audio->popen_fn) return audio->popen_fn(spawn, audio->popen_user);
    return default_popen(spawn);
}

static void clear_player(audio *audio, audio_player *player) {
    pthread_mutex_lock(&audio->mu);
    if (audio->player == player) audio->player = NULL;
    pthread_mutex_unlock(&audio->mu);
    if (player && player->destroy) player->destroy(player);
}

static void discard_player(audio_player *player) {
    if (!player) return;
    if (player->poll && player->poll(player) == -1 && player->terminate)
        player->terminate(player);
    if (player->wait) {
        int code = player->wait(player, 200);
        if (code < 0 && player->kill) {
            player->kill(player);
            if (player->wait) player->wait(player, 200);
        }
    }
    if (player->close_stdin && !player->stdin_closed) player->close_stdin(player);
    if (player->destroy) player->destroy(player);
}

static int publish_player(audio *audio, audio_player *player, atomic_int *cancelled, int generation) {
    pthread_mutex_lock(&audio->mu);
    int stopped = (cancelled && atomic_load(cancelled)) || atomic_load(&audio->stop_gen) != generation;
    audio_audible_fn fn = NULL;
    void *user = NULL;
    if (!stopped) {
        audio->player = player;
        fn = audio->audible;
        user = audio->audible_user;
    }
    pthread_mutex_unlock(&audio->mu);
    if (stopped) {
        discard_player(player);
        return -1;
    }
    if (fn) fn(user);
    return 0;
}

static int play_fd(audio *audio, int fd, atomic_int *cancelled, char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    const char *argv[] = {"pw-play", path, NULL};
    audio_spawn spawn = {.argv = argv, .argc = 2, .pass_fd = fd, .pipe_stdin = 0};
    int generation = atomic_load(&audio->stop_gen);
    if (cancelled && atomic_load(cancelled)) return 0;
    audio_player *player = open_player(audio, &spawn);
    if (!player) { set_err(err, err_cap, "Speech playback failed"); return -1; }
    if (publish_player(audio, player, cancelled, generation) != 0) return 0;
    if (player->wait) player->wait(player, -1);
    clear_player(audio, player);
    return 0;
}

typedef struct prefetch {
    audio *audio;
    const char *chunk;
    const voice_choice *choice;
    atomic_int *cancelled;
    op_result result;
    int rc;
    char error[512];
} prefetch;

static void *prefetch_main(void *arg) {
    prefetch *prefetch = arg;
    prefetch->rc = synthesize(prefetch->audio, prefetch->chunk, prefetch->choice, prefetch->cancelled,
        &prefetch->result, prefetch->error, sizeof prefetch->error);
    return NULL;
}

static const char *pcm_format(int width) {
    if (width == 1) return "u8";
    if (width == 2) return "s16";
    if (width == 3) return "s24";
    if (width == 4) return "s32";
    return NULL;
}

static int stream_chunks(audio *audio, char **chunks, size_t count, const voice_choice *choice,
    atomic_int *cancelled, char *err, size_t err_cap) {
    op_result first = {0};
    int rc = synthesize(audio, chunks[0], choice, cancelled, &first, err, err_cap);
    if (rc > 0) return 0;
    if (rc < 0) return -1;
    const char *format = pcm_format(first.width);
    if (!format) {
        free_result(&first);
        set_err(err, err_cap, "Speech playback failed");
        return -1;
    }
    char rate[16], channels[16];
    snprintf(rate, sizeof rate, "%d", first.rate);
    snprintf(channels, sizeof channels, "%d", first.channels);
    const char *argv[] = {"pw-play", "--raw", "--format", format, "--rate", rate, "--channels", channels, "-", NULL};
    audio_spawn spawn = {.argv = argv, .argc = 8, .pass_fd = -1, .pipe_stdin = 1};
    int generation = atomic_load(&audio->stop_gen);
    if (cancelled && atomic_load(cancelled)) { free_result(&first); return 0; }
    audio_player *player = open_player(audio, &spawn);
    if (!player) { free_result(&first); set_err(err, err_cap, "Speech playback failed"); return -1; }
    if (publish_player(audio, player, cancelled, generation) != 0) {
        free_result(&first);
        return 0;
    }
    unsigned char *frames = first.frames;
    size_t frame_len = first.frame_len;
    int ch = first.channels, width = first.width, sample_rate = first.rate;
    first.frames = NULL;
    prefetch pending = {0};
    int failed = 0;
    int stopped = (cancelled && atomic_load(cancelled)) || atomic_load(&audio->stop_gen) != generation;
    for (size_t i = 0; i < count && !stopped; i++) {
        stopped = (cancelled && atomic_load(cancelled)) || atomic_load(&audio->stop_gen) != generation;
        if (stopped) break;
        if (i + 1 < count) {
            pthread_t thread;
            pending = (prefetch){.audio = audio, .chunk = chunks[i + 1], .choice = choice, .cancelled = cancelled};
            if (pthread_create(&thread, NULL, prefetch_main, &pending) != 0) {
                set_err(err, err_cap, "Could not prepare the next speech chunk");
                failed = 1;
                break;
            }
            if (player->write && player->write(player, frames, frame_len) < 0) {
                pthread_join(thread, NULL);
                stopped = (cancelled && atomic_load(cancelled)) || atomic_load(&audio->stop_gen) != generation;
                if (!stopped) {
                    set_err(err, err_cap, "Speech playback stopped unexpectedly");
                    failed = 1;
                }
                free(pending.result.frames);
                break;
            }
            pthread_join(thread, NULL);
            if (pending.rc > 0) break;
            if (pending.rc < 0) {
                set_err(err, err_cap, pending.error);
                failed = 1;
                free_result(&pending.result);
                break;
            }
            if (pending.result.channels != ch || pending.result.width != width || pending.result.rate != sample_rate) {
                set_err(err, err_cap, "Speech audio format changed");
                failed = 1;
                free_result(&pending.result);
                break;
            }
            free(frames);
            frames = pending.result.frames;
            frame_len = pending.result.frame_len;
            pending.result.frames = NULL;
        } else if (player->write && player->write(player, frames, frame_len) < 0) {
            stopped = (cancelled && atomic_load(cancelled)) || atomic_load(&audio->stop_gen) != generation;
            if (!stopped) {
                set_err(err, err_cap, "Speech playback stopped unexpectedly");
                failed = 1;
            }
            break;
        }
    }
    free(frames);
    if (!stopped) stopped = (cancelled && atomic_load(cancelled)) || atomic_load(&audio->stop_gen) != generation;
    if (!failed && !stopped) {
        if (player->close_stdin) player->close_stdin(player);
        player->stdin_closed = 1;
        int code = player->wait ? player->wait(player, -1) : 0;
        stopped = (cancelled && atomic_load(cancelled)) || atomic_load(&audio->stop_gen) != generation;
        if (code && !stopped) {
            set_err(err, err_cap, "Speech playback failed");
            failed = 1;
        }
    }
    pthread_mutex_lock(&audio->mu);
    if (audio->player == player) audio->player = NULL;
    pthread_mutex_unlock(&audio->mu);
    if (player->poll && player->poll(player) == -1) {
        if (player->terminate) player->terminate(player);
        int waited = player->wait ? player->wait(player, 5000) : 0;
        if (waited < 0 && player->kill) {
            player->kill(player);
            if (player->wait) player->wait(player, -1);
        }
    }
    if (!player->stdin_closed && player->close_stdin) player->close_stdin(player);
    if (player->destroy) player->destroy(player);
    return failed ? -1 : 0;
}

static int buffered_chunks(audio *audio, char **chunks, size_t count, const voice_choice *choice,
    atomic_int *cancelled, char *err, size_t err_cap) {
    int fd = unnamed_fd(audio->runtime);
    if (fd < 0) { set_err(err, err_cap, "Could not create speech audio"); return -1; }
    if (lseek(fd, 44, SEEK_SET) < 0) { close(fd); set_err(err, err_cap, "Could not create speech audio"); return -1; }
    int started = 0, ch = 0, width = 0, rate = 0;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (cancelled && atomic_load(cancelled)) { close(fd); return 0; }
        op_result got = {0};
        int rc = synthesize(audio, chunks[i], choice, cancelled, &got, err, err_cap);
        if (rc != 0) { close(fd); return rc < 0 ? -1 : 0; }
        if (!started) { ch = got.channels; width = got.width; rate = got.rate; started = 1; }
        else if (got.channels != ch || got.width != width || got.rate != rate) {
            free_result(&got);
            close(fd);
            set_err(err, err_cap, "Speech audio format changed");
            return -1;
        }
        if (write_all(fd, got.frames, got.frame_len) != 0) {
            free_result(&got);
            close(fd);
            set_err(err, err_cap, "Could not create speech audio");
            return -1;
        }
        total += got.frame_len;
        free_result(&got);
    }
    if (!started) { close(fd); return 0; }
    unsigned char hdr[44];
    wav_header(hdr, ch, width, rate, (uint32_t)total);
    if (lseek(fd, 0, SEEK_SET) < 0 || write_all(fd, hdr, 44) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        set_err(err, err_cap, "Could not create speech audio");
        return -1;
    }
    int rc = play_fd(audio, fd, cancelled, err, err_cap);
    close(fd);
    return rc;
}

int audio_speak(audio *audio, const char *text, atomic_int *cancelled, char *err, size_t err_cap) {
    voice_choice choice;
    if (choose_voice(audio, text, &choice, err, err_cap) != 0) return -1;
    char **chunks = NULL;
    size_t count = 0;
    if (audio_speech_chunks(text ? text : "", &chunks, &count) != 0) {
        free_choice(&choice);
        set_err(err, err_cap, "Out of memory");
        return -1;
    }
    if (!count || (cancelled && atomic_load(cancelled))) {
        free_chunks(chunks, count);
        free_choice(&choice);
        return 0;
    }
    char mode[32];
    pthread_mutex_lock(&audio->mu);
    snprintf(mode, sizeof mode, "%s", audio->playback_mode ? audio->playback_mode : "buffered");
    pthread_mutex_unlock(&audio->mu);
    int rc = strcmp(mode, "streaming") == 0
        ? stream_chunks(audio, chunks, count, &choice, cancelled, err, err_cap)
        : buffered_chunks(audio, chunks, count, &choice, cancelled, err, err_cap);
    free_chunks(chunks, count);
    free_choice(&choice);
    return rc;
}

void audio_stop(audio *audio) {
    atomic_fetch_add(&audio->stop_gen, 1);
    pthread_mutex_lock(&audio->mu);
    audio_player *player = audio->player;
    if (player && player->poll && player->poll(player) == -1 && player->terminate)
        player->terminate(player);
    pthread_mutex_unlock(&audio->mu);
}

static int cue_samples(int frequency, unsigned char **out, size_t *len) {
    *len = 1280 * 2;
    *out = malloc(*len);
    if (!*out) return -1;
    for (int i = 0; i < 1280; i++) {
        double value = 750.0 * sin(2.0 * M_PI * frequency * i / 16000.0) * sin(M_PI * (double)i / 1280.0);
        int sample = (int)value;
        (*out)[i * 2] = (unsigned char)(sample & 255);
        (*out)[i * 2 + 1] = (unsigned char)((sample >> 8) & 255);
    }
    return 0;
}

static int default_run(const char *const *argv, int argc, int timeout_sec, void *user) {
    (void)argc; (void)user;
    audio_spawn spawn = {.argv = argv, .argc = argc, .pass_fd = -1, .pipe_stdin = 0};
    audio_player *player = default_popen(&spawn);
    if (!player) return 127;
    int code = player->wait(player, timeout_sec * 1000);
    if (code < 0) {
        if (player->terminate) player->terminate(player);
        if (player->kill) player->kill(player);
        if (player->wait) player->wait(player, -1);
        if (player->destroy) player->destroy(player);
        return 1;
    }
    if (player->destroy) player->destroy(player);
    return code;
}

int audio_cue(audio *audio, int frequency, char *err, size_t err_cap) {
    unsigned char *samples = NULL;
    size_t samples_len = 0;
    if (cue_samples(frequency, &samples, &samples_len) != 0) {
        set_err(err, err_cap, "Speech cue failed");
        return -1;
    }
    char tmpl[PATH_MAX];
    snprintf(tmpl, sizeof tmpl, "%s/cueXXXXXX", audio->runtime);
    int fd = mkstemp(tmpl);
    if (fd < 0) { free(samples); set_err(err, err_cap, "Speech cue failed"); return -1; }
    unsigned char hdr[44];
    wav_header(hdr, 1, 2, 16000, (uint32_t)samples_len);
    int wrote = write_all(fd, hdr, 44) == 0 && write_all(fd, samples, samples_len) == 0;
    close(fd);
    free(samples);
    if (!wrote) { unlink(tmpl); set_err(err, err_cap, "Speech cue failed"); return -1; }
    const char *argv[] = {"pw-play", tmpl, NULL};
    int rc = audio->run_fn ? audio->run_fn(argv, 2, 3, audio->run_user) : default_run(argv, 2, 3, NULL);
    unlink(tmpl);
    if (rc == 0) return 0;
    set_err(err, err_cap, rc == 1 ? "Speech cue timed out" : "Speech cue failed");
    return -1;
}

void *audio_start_capture(audio *audio, const char *path, char *err, size_t err_cap) {
    if (!audio->mic_resolve || !audio->capture) {
        set_err(err, err_cap, "Capture is not available");
        return NULL;
    }
    char target[128] = {0}, name[128] = {0};
    if (audio->mic_resolve(audio->mic_user, audio->preferred_microphone, target, sizeof target, name, sizeof name) != 0) {
        set_err(err, err_cap, "Microphone is not available");
        return NULL;
    }
    return audio->capture(path, target, audio->capture_user);
}

typedef struct call_op { int (*op)(void *user, char *err, size_t err_cap); void *user; } call_op;

static int call_op_run(void *user, op_result *result) {
    call_op *call = user;
    return call->op(call->user, result->error, sizeof result->error);
}
static void free_call_op(void *user) { free(user); }

int audio_call(audio *audio, atomic_int *cancelled,
    int (*op)(void *user, char *err, size_t err_cap), void *user, char *err, size_t err_cap) {
    call_op *call = calloc(1, sizeof *call);
    if (!call) { set_err(err, err_cap, "Out of memory"); return -1; }
    call->op = op;
    call->user = user;
    op_result result = {0};
    int rc = run_cancellable(audio, &audio->call_mu, cancelled, call_op_run, call, free_call_op,
        NULL, NULL, NULL, NULL, &result, err, err_cap);
    free_result(&result);
    return rc;
}
