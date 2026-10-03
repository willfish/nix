#define _POSIX_C_SOURCE 200809L
#include "audio.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
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
    SPEAK_MAX = 8 * 1024 * 1024,
    WHISPER_MAX = 1024 * 1024,
    TTS_MAX = 32 * 1024 * 1024,
    HEALTH_MAX = 16384,
    CHUNK_FIRST = 120,
    CHUNK_MAX = 260
};

extern char **environ;

static const char *DEEPGRAM_NAMES[] = {
    "amalthea", "andromeda", "apollo", "arcas", "aries",
    "asteria", "athena", "atlas", "aurora", "callista",
    "cora", "cordelia", "delia", "draco", "electra",
    "harmonia", "helena", "hera", "hermes", "hyperion",
    "iris", "janus", "juno", "jupiter", "luna", "mars",
    "minerva", "neptune", "odysseus", "ophelia", "orion",
    "orpheus", "pandora", "phoebe", "pluto", "saturn",
    "selene", "thalia", "theia", "vesta", "zeus"
};

typedef struct copied_voice {
    char *id;
    char *label;
    audio_pair *options;
    size_t option_count;
} copied_voice;

typedef struct voice_choice {
    int deepgram;
    char model[96];
    char tts_model[128];
    audio_pair *pairs;
    size_t npairs;
} voice_choice;

struct audio {
    char *runtime;
    char *stt_url;
    char *stt_health;
    char *tts_url;
    char *tts_health;
    char *stt_prompt;
    char *stt_language;
    char *tts_model;
    char *preferred_microphone;
    char *voice_preferences;
    char *deepgram_preferences;
    char *speech_preferences;
    char *stt_preferences;
    char *playback_mode;
    copied_voice *voices;
    size_t voice_count;
    audio_pair *long_voice;
    size_t long_voice_count;
    int has_local;
    int has_deepgram_speech;
    double readiness_timeout;
    char selected_voice[64];
    char deepgram_voice[64];
    char speech_backend[32];
    char stt_backend[32];
    char stt_state[32];
    char tts_state[32];
    char stt_error[512];
    char tts_error[512];
    int has_stt_error;
    int has_tts_error;
    double stt_updated;
    double tts_updated;
    int refreshing_stt;
    int refreshing_tts;
    audio_http_fn http;
    void *http_user;
    audio_popen_fn popen_fn;
    void *popen_user;
    audio_run_fn run_fn;
    void *run_user;
    audio_acquire_fn acquire;
    void *acquire_user;
    audio_audible_fn audible;
    void *audible_user;
    audio_mic_status_fn mic_status;
    audio_mic_resolve_fn mic_resolve;
    void *mic_user;
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
    const char *engine;
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
    pthread_mutex_t mu;
    pthread_cond_t cv;
} job;

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

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int sleep_cancel(atomic_int *cancelled, double seconds) {
    double left = seconds;
    while (left > 0) {
        if (cancelled && atomic_load(cancelled)) return 1;
        int slice = left > 0.01 ? 10 : (int)(left * 1000);
        if (slice < 1) slice = 1;
        sleep_ms(slice);
        left -= slice / 1000.0;
    }
    return cancelled && atomic_load(cancelled);
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
    if (fprintf(file, "%s\n", value) < 0 || fclose(file) != 0) {
        fclose(file);
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

static int has_deepgram_key(void) {
    const char *key = getenv("DEEPGRAM_API_KEY");
    return key && key[0];
}

static char *copy_key(void) {
    const char *key = getenv("DEEPGRAM_API_KEY");
    if (!key || !key[0]) return NULL;
    return strdup(key);
}

static const char *engine_name(const char *engine) {
    return engine && strcmp(engine, "stt") == 0 ? "Whisper" : "Samantha TTS";
}

static int deepgram_known(const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < sizeof DEEPGRAM_NAMES / sizeof DEEPGRAM_NAMES[0]; i++) {
        if (strcmp(DEEPGRAM_NAMES[i], name) == 0) return 1;
    }
    return 0;
}

static int local_index(const audio *audio, const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < audio->voice_count; i++) {
        if (strcmp(audio->voices[i].id, name) == 0) return (int)i;
    }
    return -1;
}

static void set_backend(audio *audio, const char *engine, const char *state, const char *error) {
    pthread_mutex_lock(&audio->backend_mu);
    char *slot = strcmp(engine, "stt") == 0 ? audio->stt_state : audio->tts_state;
    char *err = strcmp(engine, "stt") == 0 ? audio->stt_error : audio->tts_error;
    int *has = strcmp(engine, "stt") == 0 ? &audio->has_stt_error : &audio->has_tts_error;
    double *updated = strcmp(engine, "stt") == 0 ? &audio->stt_updated : &audio->tts_updated;
    snprintf(slot, 32, "%s", state);
    *updated = mono_now();
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
        bytes[0] = (unsigned char)(0xE0 | (code >> 12));
        bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (code & 0x3F));
        len = 3;
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
                if (p + 4 > end) { free(out); return NULL; }
                unsigned code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = *p++;
                    code <<= 4;
                    if (h >= '0' && h <= '9') code += (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') code += (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code += (unsigned)(h - 'A' + 10);
                    else { free(out); return NULL; }
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
        if (utf8_append(&out, &n, &cap, c) != 0) { free(out); return NULL; }
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
    unsigned char **frames, size_t *frame_len) {
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
            ch = rd16(data + off + 2);
            sample_rate = (int)rd32(data + off + 4);
            bits = rd16(data + off + 14);
            got_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            declared = size;
            payload = data + off;
            available = have < size ? have : size;
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

static pcre2_code *sentence_re;
static pcre2_code *clause_re;

static void ensure_chunk_re(void) {
    if (sentence_re || clause_re) return;
    int error = 0;
    PCRE2_SIZE offset = 0;
    sentence_re = pcre2_compile((PCRE2_SPTR)"[.!?][\"')\\]]*(?=\\s|$)", PCRE2_ZERO_TERMINATED, 0, &error, &offset, NULL);
    clause_re = pcre2_compile((PCRE2_SPTR)"[,;:][\"')\\]]*(?=\\s|$)", PCRE2_ZERO_TERMINATED, 0, &error, &offset, NULL);
}

static char *normalize(const char *src) {
    size_t cap = strlen(src) + 1;
    if (cap > 1024 * 1024) return NULL;
    char *dst = malloc(cap);
    if (!dst) return NULL;
    size_t n = 0;
    int space = 1;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (isspace(*p)) {
            if (!space) dst[n++] = ' ';
            space = 1;
        } else {
            dst[n++] = (char)*p;
            space = 0;
        }
    }
    if (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
    return dst;
}

static int last_boundary(pcre2_code *code, const char *window, size_t window_len, size_t limit, size_t *end) {
    if (!code) return 0;
    pcre2_match_data *data = pcre2_match_data_create(4, NULL);
    if (!data) return 0;
    PCRE2_SIZE start = 0;
    size_t shorter = 0, first = 0;
    int found = 0, have_shorter = 0;
    while (start <= window_len) {
        int rc = pcre2_match(code, (PCRE2_SPTR)window, window_len, start, 0, data, NULL);
        if (rc < 0) break;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(data);
        if (ov[1] <= CHUNK_MAX) {
            if (!found) first = ov[1];
            found = 1;
            if (ov[1] <= limit) { shorter = ov[1]; have_shorter = 1; }
        }
        PCRE2_SIZE next = ov[1] > start ? ov[1] : start + 1;
        if (next <= start) break;
        start = next;
    }
    pcre2_match_data_free(data);
    if (!found) return 0;
    *end = have_shorter ? shorter : first;
    return 1;
}

int audio_speech_chunks(const char *text, char ***out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (!text || !text[0]) return 0;
    ensure_chunk_re();
    char *remaining = normalize(text);
    if (!remaining) return -1;
    size_t cap = 8;
    char **chunks = calloc(cap, sizeof *chunks);
    if (!chunks) { free(remaining); return -1; }
    while (remaining[0]) {
        if (*count + 1 > cap) {
            size_t next = cap * 2;
            char **grown = realloc(chunks, next * sizeof *chunks);
            if (!grown) { free(remaining); return -1; }
            chunks = grown;
            cap = next;
        }
        size_t len = strlen(remaining);
        size_t limit = *count ? CHUNK_MAX : CHUNK_FIRST;
        if (len <= limit) {
            chunks[*count] = strdup(remaining);
            if (!chunks[*count]) { free(remaining); return -1; }
            (*count)++;
            break;
        }
        size_t window = len < CHUNK_MAX + 1 ? len : CHUNK_MAX + 1;
        size_t end = 0;
        if (!last_boundary(sentence_re, remaining, window, limit, &end)
            && !last_boundary(clause_re, remaining, window, limit, &end)) {
            size_t last = 0;
            int found = 0;
            for (size_t i = 0; i < limit + 1 && i < len; i++) {
                if (remaining[i] == ' ') { last = i; found = 1; }
            }
            end = found && last > 0 ? last : limit;
        }
        if (end == 0) end = limit ? limit : 1;
        size_t cut = end;
        while (cut && isspace((unsigned char)remaining[cut - 1])) cut--;
        if (cut == 0) cut = end;
        chunks[*count] = strndup(remaining, cut);
        if (!chunks[*count]) { free(remaining); return -1; }
        (*count)++;
        const char *next = remaining + end;
        while (*next && isspace((unsigned char)*next)) next++;
        memmove(remaining, next, strlen(next) + 1);
    }
    free(remaining);
    *out = chunks;
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
    curl_buf buf = {.max = request->timeout_ms > 0 ? SPEAK_MAX + 1 : SPEAK_MAX + 1};
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
    response->body_len = buf.len;
    response->transport_error = rc != CURLE_OK;
    if (buf.overflow) response->body_len = buf.max + 1;
    return 0;
}

static int http_call(audio *audio, const audio_http_request *request, audio_http_response *response) {
    memset(response, 0, sizeof *response);
    if (audio->http) return audio->http(request, response, audio->http_user);
    return http_curl(request, response);
}

static int probe(audio *audio, const char *engine, int timeout_ms, char *err, size_t err_cap) {
    const char *url = strcmp(engine, "stt") == 0 ? audio->stt_health : audio->tts_health;
    if (!url) return 1;
    audio_http_request request = {.method = "GET", .url = url, .timeout_ms = timeout_ms};
    audio_http_response response;
    http_call(audio, &request, &response);
    if (response.verbatim_error[0]) {
        set_err(err, err_cap, response.verbatim_error);
        free(response.body);
        return -1;
    }
    if (response.transport_error) { free(response.body); return 0; }
    if (response.status == 503) { free(response.body); return 0; }
    if (response.status < 200 || response.status >= 300) {
        snprintf(err, err_cap, "%s health endpoint returned HTTP %d; check pi-voice-%s.service",
            engine_name(engine), response.status, engine);
        free(response.body);
        return -1;
    }
    size_t take = response.body_len > HEALTH_MAX ? HEALTH_MAX : response.body_len;
    json_value *root = json_parse(response.body, take);
    free(response.body);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        snprintf(err, err_cap, "%s returned invalid health data", engine_name(engine));
        return -1;
    }
    if (strcmp(engine, "stt") == 0) {
        json_value *status = json_get(root, "status");
        int ok = status && status->type == JSON_STRING && status->string && strcmp(status->string, "ok") == 0;
        json_free(root);
        return ok ? 1 : 0;
    }
    json_value *data = json_get(root, "data");
    if (!data || data->type != JSON_ARRAY) {
        json_free(root);
        set_err(err, err_cap, "Samantha TTS returned invalid model data");
        return -1;
    }
    const char *model_id = audio->tts_model ? audio->tts_model : "pi-voice";
    int found = 0, loaded = 0;
    for (size_t i = 0; i < data->nitems; i++) {
        json_value *model = data->items[i];
        if (!model || model->type != JSON_OBJECT) continue;
        json_value *id = json_get(model, "id");
        if (!id || id->type != JSON_STRING || !id->string || strcmp(id->string, model_id) != 0) continue;
        found = 1;
        json_value *flag = json_get(model, "loaded");
        loaded = flag && flag->type == JSON_BOOL && flag->bool_value;
        break;
    }
    json_free(root);
    if (!found) {
        snprintf(err, err_cap, "Samantha TTS model '%s' is missing from the server; check pi-voice-tts.service and model files", model_id);
        return -1;
    }
    return loaded ? 1 : 0;
}

static int abandoned(audio *audio, atomic_int *cancelled, int stop_gen) {
    if (cancelled && atomic_load(cancelled)) return 1;
    return atomic_load(&audio->stop_gen) != stop_gen;
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
    audio_lease *lease = NULL;
    int got_gate = 0;
    if (job->engine && job->engine[0] && owner->acquire) {
        lease = owner->acquire(job->engine, owner->acquire_user);
        if (!lease) {
            set_err(job->result.error, sizeof job->result.error, "engine acquire failed");
            job->result.status = -1;
            goto publish;
        }
        while (!abandoned(owner, job->cancelled, job->stop_gen)) {
            char wait_err[512] = {0};
            int wr = lease->wait_ms ? lease->wait_ms(lease, 50, wait_err, sizeof wait_err) : 0;
            if (wr == 0) break;
            if (wr == 1) {
                if (lease->ready_done && lease->ready_done(lease)) {
                    set_err(job->result.error, sizeof job->result.error, "engine lease timed out");
                    job->result.status = -1;
                    goto publish;
                }
                continue;
            }
            set_err(job->result.error, sizeof job->result.error, wait_err[0] ? wait_err : "engine lease failed");
            job->result.status = -1;
            goto publish;
        }
        if (abandoned(owner, job->cancelled, job->stop_gen)) goto publish;
    }
    while (!abandoned(owner, job->cancelled, job->stop_gen)) {
        int rc = lock_timeout(job->gate, 50);
        if (rc == 0) { got_gate = 1; break; }
        if (rc != ETIMEDOUT) {
            set_err(job->result.error, sizeof job->result.error, "Could not acquire the speech lock");
            job->result.status = -1;
            goto publish;
        }
    }
    if (!got_gate) goto publish;
    if (!abandoned(owner, job->cancelled, job->stop_gen)) {
        int rc = job->op(job->user, &job->result);
        if (rc != 0) job->result.status = -1;
    }
    pthread_mutex_unlock(job->gate);
    got_gate = 0;
publish:
    if (got_gate) pthread_mutex_unlock(job->gate);
    if (lease && lease->release) lease->release(lease);
    if (job->free_user) job->free_user(job->user);
    job->user = NULL;
    if (job->result.status == 0 && job->on_drained && job->result.text)
        job->on_drained(job->result.text, job->drain_user);
    pthread_mutex_lock(&job->mu);
    if (job->left) {
        pthread_mutex_unlock(&job->mu);
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

static int run_cancellable(audio *audio, pthread_mutex_t *gate, const char *engine, atomic_int *cancelled,
    int (*op)(void *user, op_result *result), void *user, void (*free_user)(void *user),
    audio_drained_fn on_drained, void *drain_user, op_result *result, char *err, size_t err_cap) {
    job *job = calloc(1, sizeof *job);
    if (!job) { if (free_user) free_user(user); set_err(err, err_cap, "Out of memory"); return -1; }
    job->audio = audio;
    job->gate = gate;
    job->engine = engine;
    job->cancelled = cancelled;
    job->stop_gen = atomic_load(&audio->stop_gen);
    job->op = op;
    job->user = user;
    job->free_user = free_user;
    job->on_drained = on_drained;
    job->drain_user = drain_user;
    pthread_mutex_init(&job->mu, NULL);
    pthread_cond_init(&job->cv, NULL);
    if (spawn_detached(audio, job_main, job) != 0) {
        if (free_user) free_user(user);
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
    pthread_mutex_destroy(&job->mu);
    pthread_cond_destroy(&job->cv);
    free(job);
    if (was_abandoned) return 1;
    if (status != 0) { set_err(err, err_cap, error_copy); return -1; }
    return 0;
}

typedef struct refresh_arg { audio *audio; char *engine; } refresh_arg;

static void *refresh_main(void *arg) {
    refresh_arg *refresh = arg;
    audio *audio = refresh->audio;
    char err[512];
    int ready = probe(audio, refresh->engine, 250, err, sizeof err);
    if (ready < 0) set_backend(audio, refresh->engine, "error", err);
    else set_backend(audio, refresh->engine, ready ? "ready" : "loading", NULL);
    pthread_mutex_lock(&audio->backend_mu);
    if (strcmp(refresh->engine, "stt") == 0) audio->refreshing_stt = 0;
    else audio->refreshing_tts = 0;
    pthread_mutex_unlock(&audio->backend_mu);
    free(refresh->engine);
    free(refresh);
    live_done(audio);
    return NULL;
}

static void maybe_refresh(audio *audio, const char *engine) {
    const char *url = strcmp(engine, "stt") == 0 ? audio->stt_health : audio->tts_health;
    int *flag = strcmp(engine, "stt") == 0 ? &audio->refreshing_stt : &audio->refreshing_tts;
    double updated = strcmp(engine, "stt") == 0 ? audio->stt_updated : audio->tts_updated;
    if (!url || *flag || mono_now() - updated <= 2) return;
    refresh_arg *arg = calloc(1, sizeof *arg);
    if (!arg) return;
    arg->audio = audio;
    arg->engine = strdup(engine);
    if (!arg->engine) { free(arg); return; }
    *flag = 1;
    if (spawn_detached(audio, refresh_main, arg) != 0) {
        *flag = 0;
        free(arg->engine);
        free(arg);
    }
}

void audio_config_init(audio_config *config) {
    memset(config, 0, sizeof *config);
    config->tts_enabled = 1;
}

static audio_pair *copy_pairs(const audio_pair *pairs, size_t count) {
    if (!count) return NULL;
    audio_pair *out = calloc(count, sizeof *out);
    if (!out) return NULL;
    for (size_t i = 0; i < count; i++) {
        out[i].key = strdup(pairs[i].key ? pairs[i].key : "");
        out[i].value = strdup(pairs[i].value ? pairs[i].value : "");
        if (!out[i].key || !out[i].value) return out;
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
    audio->stt_url = dup_opt(config->stt_url);
    audio->stt_health = dup_opt(config->stt_health_url);
    audio->tts_url = dup_opt(config->tts_url);
    audio->tts_health = dup_opt(config->tts_health_url);
    audio->stt_prompt = dup_opt(config->stt_prompt);
    audio->stt_language = strdup(config->stt_language && config->stt_language[0] ? config->stt_language : "en");
    audio->tts_model = strdup(config->tts_model && config->tts_model[0] ? config->tts_model : "pi-voice");
    audio->preferred_microphone = dup_opt(config->preferred_microphone);
    audio->voice_preferences = dup_opt(config->voice_preferences_path);
    audio->playback_mode = strdup(config->playback_mode && config->playback_mode[0] ? config->playback_mode : "buffered");
    audio->readiness_timeout = config->readiness_timeout > 0 ? config->readiness_timeout : 60;
    audio->has_local = config->tts_enabled ? 1 : 0;
    audio->has_deepgram_speech = has_deepgram_key();
    snprintf(audio->selected_voice, sizeof audio->selected_voice, "samantha");
    snprintf(audio->deepgram_voice, sizeof audio->deepgram_voice, "thalia");
    snprintf(audio->speech_backend, sizeof audio->speech_backend, "%s", audio->has_deepgram_speech ? "deepgram" : "local");
    snprintf(audio->stt_backend, sizeof audio->stt_backend, "whisper");
    snprintf(audio->stt_state, sizeof audio->stt_state, "unknown");
    snprintf(audio->tts_state, sizeof audio->tts_state, "unknown");
    if (audio->voice_preferences) {
        audio->deepgram_preferences = sibling_name(audio->voice_preferences, "deepgram-voice");
        audio->speech_preferences = sibling_name(audio->voice_preferences, "speech-backend");
    }
    audio->stt_preferences = dup_opt(config->stt_preferences_path);
    if (!audio->stt_preferences && audio->voice_preferences)
        audio->stt_preferences = sibling_name(audio->voice_preferences, "stt-backend");
    size_t extras = audio->has_local ? config->voice_count : 0;
    audio->voice_count = audio->has_local ? extras + 1 : 0;
    if (audio->voice_count) {
        audio->voices = calloc(audio->voice_count, sizeof *audio->voices);
        audio->voices[0].id = strdup("samantha");
        audio->voices[0].label = strdup("Samantha");
        for (size_t i = 0; i < extras; i++) {
            audio->voices[i + 1].id = strdup(config->voices[i].id ? config->voices[i].id : "");
            audio->voices[i + 1].label = strdup(config->voices[i].label ? config->voices[i].label : audio->voices[i + 1].id);
            audio->voices[i + 1].options = copy_pairs(config->voices[i].options, config->voices[i].option_count);
            audio->voices[i + 1].option_count = config->voices[i].option_count;
        }
    }
    if (config->long_voice_count) {
        audio->long_voice = copy_pairs(config->long_voice, config->long_voice_count);
        audio->long_voice_count = config->long_voice_count;
    }
    char *saved = read_pref(audio->voice_preferences);
    if (saved && local_index(audio, saved) >= 0)
        snprintf(audio->selected_voice, sizeof audio->selected_voice, "%s", saved);
    free(saved);
    saved = read_pref(audio->deepgram_preferences);
    if (saved && deepgram_known(saved))
        snprintf(audio->deepgram_voice, sizeof audio->deepgram_voice, "%s", saved);
    free(saved);
    saved = read_pref(audio->speech_preferences);
    if (saved && ((strcmp(saved, "local") == 0 && audio->has_local) || (strcmp(saved, "deepgram") == 0 && audio->has_deepgram_speech)))
        snprintf(audio->speech_backend, sizeof audio->speech_backend, "%s", saved);
    free(saved);
    saved = read_pref(audio->stt_preferences);
    if (saved && (strcmp(saved, "whisper") == 0 || strcmp(saved, "deepgram") == 0))
        snprintf(audio->stt_backend, sizeof audio->stt_backend, "%s", saved);
    free(saved);
    pthread_mutex_init(&audio->mu, NULL);
    pthread_mutex_init(&audio->backend_mu, NULL);
    pthread_mutex_init(&audio->synthesis_mu, NULL);
    pthread_mutex_init(&audio->recognition_mu, NULL);
    pthread_mutex_init(&audio->call_mu, NULL);
    pthread_mutex_init(&audio->live_mu, NULL);
    pthread_cond_init(&audio->live_cv, NULL);
    return audio;
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
    free(audio->stt_url);
    free(audio->stt_health);
    free(audio->tts_url);
    free(audio->tts_health);
    free(audio->stt_prompt);
    free(audio->stt_language);
    free(audio->tts_model);
    free(audio->preferred_microphone);
    free(audio->voice_preferences);
    free(audio->deepgram_preferences);
    free(audio->speech_preferences);
    free(audio->stt_preferences);
    free(audio->playback_mode);
    for (size_t i = 0; i < audio->voice_count; i++) {
        free(audio->voices[i].id);
        free(audio->voices[i].label);
        free_pairs(audio->voices[i].options, audio->voices[i].option_count);
    }
    free(audio->voices);
    free_pairs(audio->long_voice, audio->long_voice_count);
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
void audio_set_engines(audio *audio, audio_acquire_fn fn, void *user) { audio->acquire = fn; audio->acquire_user = user; }
void audio_set_audible(audio *audio, audio_audible_fn fn, void *user) { audio->audible = fn; audio->audible_user = user; }
void audio_set_microphone(audio *audio, audio_mic_status_fn status, audio_mic_resolve_fn resolve, void *user) {
    audio->mic_status = status; audio->mic_resolve = resolve; audio->mic_user = user;
}
void audio_set_capture(audio *audio, audio_capture_fn fn, void *user) { audio->capture = fn; audio->capture_user = user; }
void audio_set_playback_mode(audio *audio, const char *mode) {
    pthread_mutex_lock(&audio->mu);
    free(audio->playback_mode);
    audio->playback_mode = strdup(mode && mode[0] ? mode : "buffered");
    pthread_mutex_unlock(&audio->mu);
}

static void unknown_voice(audio *audio, int deepgram, char *err, size_t cap) {
    if (!err || !cap) return;
    size_t used = (size_t)snprintf(err, cap, "Unknown voice; choose ");
    if (deepgram) {
        for (size_t i = 0; i < sizeof DEEPGRAM_NAMES / sizeof DEEPGRAM_NAMES[0] && used < cap; i++)
            used += (size_t)snprintf(err + used, cap - used, "%s%s", i ? ", " : "", DEEPGRAM_NAMES[i]);
    } else {
        for (size_t i = 0; i < audio->voice_count && used < cap; i++)
            used += (size_t)snprintf(err + used, cap - used, "%s%s", i ? ", " : "", audio->voices[i].id);
    }
}

int audio_set_speech_backend(audio *audio, const char *backend, char *err, size_t err_cap) {
    if (!backend || (strcmp(backend, "local") != 0 && strcmp(backend, "deepgram") != 0)
        || (strcmp(backend, "local") == 0 && !audio->has_local)
        || (strcmp(backend, "deepgram") == 0 && !audio->has_deepgram_speech)) {
        set_err(err, err_cap, "That speech backend is not available");
        return -1;
    }
    pthread_mutex_lock(&audio->mu);
    if (save_pref(audio->speech_preferences, backend) != 0) {
        pthread_mutex_unlock(&audio->mu);
        set_err(err, err_cap, "Could not save the speech backend");
        return -1;
    }
    snprintf(audio->speech_backend, sizeof audio->speech_backend, "%s", backend);
    pthread_mutex_unlock(&audio->mu);
    return 0;
}

int audio_set_voice(audio *audio, const char *character, char *err, size_t err_cap) {
    pthread_mutex_lock(&audio->mu);
    int deepgram = strcmp(audio->speech_backend, "deepgram") == 0;
    if (deepgram) {
        if (!deepgram_known(character)) {
            unknown_voice(audio, 1, err, err_cap);
            pthread_mutex_unlock(&audio->mu);
            return -1;
        }
        if (save_pref(audio->deepgram_preferences, character) != 0) {
            pthread_mutex_unlock(&audio->mu);
            set_err(err, err_cap, "Could not save the voice");
            return -1;
        }
        snprintf(audio->deepgram_voice, sizeof audio->deepgram_voice, "%s", character);
    } else {
        if (local_index(audio, character) < 0) {
            unknown_voice(audio, 0, err, err_cap);
            pthread_mutex_unlock(&audio->mu);
            return -1;
        }
        if (save_pref(audio->voice_preferences, character) != 0) {
            pthread_mutex_unlock(&audio->mu);
            set_err(err, err_cap, "Could not save the voice");
            return -1;
        }
        snprintf(audio->selected_voice, sizeof audio->selected_voice, "%s", character);
    }
    pthread_mutex_unlock(&audio->mu);
    return 0;
}

int audio_set_stt_backend(audio *audio, const char *backend, char *err, size_t err_cap) {
    if (!backend || (strcmp(backend, "whisper") != 0 && strcmp(backend, "deepgram") != 0)) {
        set_err(err, err_cap, "Unknown dictation backend");
        return -1;
    }
    if (strcmp(backend, "deepgram") == 0 && !has_deepgram_key()) {
        set_err(err, err_cap, "Deepgram API key is not installed; stay on Whisper");
        return -1;
    }
    pthread_mutex_lock(&audio->mu);
    if (save_pref(audio->stt_preferences, backend) != 0) {
        pthread_mutex_unlock(&audio->mu);
        set_err(err, err_cap, "Could not save the dictation backend");
        return -1;
    }
    snprintf(audio->stt_backend, sizeof audio->stt_backend, "%s", backend);
    pthread_mutex_unlock(&audio->mu);
    return 0;
}

int audio_status(audio *audio, audio_report *status) {
    memset(status, 0, sizeof *status);
    pthread_mutex_lock(&audio->mu);
    snprintf(status->speech_backend, sizeof status->speech_backend, "%s", audio->speech_backend);
    snprintf(status->selected_stt, sizeof status->selected_stt, "%s", audio->stt_backend);
    int deepgram = strcmp(audio->speech_backend, "deepgram") == 0;
    if (deepgram) {
        snprintf(status->selected_voice, sizeof status->selected_voice, "%s", audio->deepgram_voice);
        status->voice_count = sizeof DEEPGRAM_NAMES / sizeof DEEPGRAM_NAMES[0];
        for (size_t i = 0; i < status->voice_count && i < 48; i++) {
            snprintf(status->voice_ids[i], sizeof status->voice_ids[i], "%s", DEEPGRAM_NAMES[i]);
            snprintf(status->voice_labels[i], sizeof status->voice_labels[i], "%c%s",
                toupper((unsigned char)DEEPGRAM_NAMES[i][0]), DEEPGRAM_NAMES[i] + 1);
        }
    } else {
        snprintf(status->selected_voice, sizeof status->selected_voice, "%s", audio->selected_voice);
        status->voice_count = audio->voice_count < 48 ? audio->voice_count : 48;
        for (size_t i = 0; i < status->voice_count; i++) {
            snprintf(status->voice_ids[i], sizeof status->voice_ids[i], "%s", audio->voices[i].id);
            snprintf(status->voice_labels[i], sizeof status->voice_labels[i], "%s", audio->voices[i].label);
        }
    }
    if (audio->has_local) {
        snprintf(status->speech_backend_ids[status->speech_backend_count], 32, "local");
        snprintf(status->speech_backend_labels[status->speech_backend_count], 64, "Local characters");
        status->speech_backend_count++;
    }
    if (audio->has_deepgram_speech) {
        snprintf(status->speech_backend_ids[status->speech_backend_count], 32, "deepgram");
        snprintf(status->speech_backend_labels[status->speech_backend_count], 64, "Deepgram");
        status->speech_backend_count++;
    }
    int hide_tts = deepgram || !audio->has_local;
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
    if (status->has_stt) maybe_refresh(audio, "stt");
    if (status->has_tts) maybe_refresh(audio, "tts");
    pthread_mutex_unlock(&audio->backend_mu);
    snprintf(status->stt_ids[0], 32, "whisper");
    snprintf(status->stt_labels[0], 64, "Whisper (local GPU)");
    snprintf(status->stt_ids[1], 32, "deepgram");
    snprintf(status->stt_labels[1], 64, "Deepgram (cloud)");
    status->stt_count = 2;
    if (audio->mic_status)
        audio->mic_status(audio->mic_user, status->microphone_name, sizeof status->microphone_name,
            status->microphone_target, sizeof status->microphone_target);
    return 0;
}

int audio_playing(audio *audio) {
    pthread_mutex_lock(&audio->mu);
    int playing = audio->player != NULL;
    pthread_mutex_unlock(&audio->mu);
    return playing;
}

int audio_wait_ready(audio *audio, const char *engine, atomic_int *cancelled, char *err, size_t err_cap) {
    const char *url = strcmp(engine, "stt") == 0 ? audio->stt_health : audio->tts_health;
    if (!url) return cancelled && atomic_load(cancelled) ? 0 : 1;
    double budget = audio->readiness_timeout > 0 ? audio->readiness_timeout : 60;
    double start = mono_now();
    set_backend(audio, engine, "loading", NULL);
    for (;;) {
        if (cancelled && atomic_load(cancelled)) return 0;
        double remaining = budget - (mono_now() - start);
        if (remaining <= 0) {
            snprintf(err, err_cap, "%s model is not ready; check pi-voice-%s.service and model files",
                engine_name(engine), engine);
            set_backend(audio, engine, "error", err);
            return -1;
        }
        int probe_ms = remaining < 0.25 ? (int)(remaining * 1000) : 250;
        if (probe_ms < 1) probe_ms = 1;
        char perr[512] = {0};
        int ready = probe(audio, engine, probe_ms, perr, sizeof perr);
        if (ready < 0) {
            set_backend(audio, engine, "error", perr);
            set_err(err, err_cap, perr);
            return -1;
        }
        if (ready) {
            set_backend(audio, engine, "ready", NULL);
            return cancelled && atomic_load(cancelled) ? 0 : 1;
        }
        if (sleep_cancel(cancelled, remaining < 0.1 ? remaining : 0.1)) return 0;
    }
}

static int parse_wav_result(const unsigned char *data, size_t len, int deepgram, op_result *result) {
    int channels = 0, width = 0, rate = 0;
    unsigned char *frames = NULL;
    size_t frame_len = 0;
    if (wav_parse(data, len, &channels, &width, &rate, &frames, &frame_len) != 0 || !frames || frame_len == 0
        || width <= 0 || channels <= 0 || frame_len % (size_t)(channels * width) != 0) {
        free(frames);
        set_err(result->error, sizeof result->error,
            deepgram ? "Deepgram returned incomplete speech audio" : "Incomplete speech audio");
        return -1;
    }
    if (!deepgram) {
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
    int deepgram_speak;
    int deepgram_listen;
    char *engine;
} http_op;

static void free_http_op(void *user) {
    http_op *op = user;
    if (!op) return;
    free(op->url);
    free(op->content_type);
    free(op->authorization);
    free(op->body);
    free(op->engine);
    free(op);
}

static int http_op_run(void *user, op_result *result) {
    http_op *op = user;
    audio_http_request request = {
        .method = "POST", .url = op->url, .content_type = op->content_type,
        .authorization = op->authorization, .body = op->body, .body_len = op->body_len,
        .timeout_ms = op->timeout_ms
    };
    audio_http_response response;
    http_call(op->audio, &request, &response);
    if (response.verbatim_error[0]) {
        set_err(result->error, sizeof result->error, response.verbatim_error);
        free(response.body);
        return -1;
    }
    if (op->deepgram_speak || op->deepgram_listen) {
        const char *what = op->deepgram_speak ? "Deepgram speech" : "Deepgram";
        if (response.transport_error || response.status == 0) {
            snprintf(result->error, sizeof result->error, "%s connection failed", what);
            free(response.body);
            return -1;
        }
        if (response.status >= 400) {
            snprintf(result->error, sizeof result->error, "%s %s (HTTP %d)",
                what, op->deepgram_speak ? "failed" : "request failed", response.status);
            free(response.body);
            return -1;
        }
        if (response.body_len > op->maximum) {
            set_err(result->error, sizeof result->error,
                op->deepgram_speak ? "Deepgram speech response is too large" : "Deepgram response exceeds the size limit");
            free(response.body);
            return -1;
        }
        if (op->deepgram_listen) {
            json_value *root = json_parse(response.body, response.body_len);
            free(response.body);
            if (!root) {
                set_err(result->error, sizeof result->error, "Deepgram returned invalid JSON");
                return -1;
            }
            int rc = 0;
            if (!root || root->type != JSON_OBJECT) {
                set_err(result->error, sizeof result->error, "Deepgram returned invalid JSON");
                rc = -1;
            } else {
                json_value *text = json_get(root, "text");
                if (text && text->type == JSON_STRING && text->string && strspn(text->string, " \t\r\n") != strlen(text->string))
                    result->text = strdup(text->string);
                else {
                    json_value *results = json_get(root, "results");
                    json_value *channels = results ? json_get(results, "channels") : NULL;
                    json_value *alt = NULL;
                    if (channels && channels->type == JSON_ARRAY && channels->nitems && channels->items[0]->type == JSON_OBJECT) {
                        json_value *alts = json_get(channels->items[0], "alternatives");
                        if (alts && alts->type == JSON_ARRAY && alts->nitems && alts->items[0]->type == JSON_OBJECT)
                            alt = json_get(alts->items[0], "transcript");
                    }
                    if (!alt || alt->type != JSON_STRING) {
                        set_err(result->error, sizeof result->error, "Deepgram returned no transcription text");
                        rc = -1;
                    } else result->text = strdup(alt->string ? alt->string : "");
                }
            }
            json_free(root);
            if (rc == 0 && !result->text) { set_err(result->error, sizeof result->error, "Out of memory"); return -1; }
            return rc;
        }
        int rc = parse_wav_result(response.body, response.body_len, 1, result);
        free(response.body);
        return rc;
    }
    if (response.transport_error || response.status == 0) {
        snprintf(result->error, sizeof result->error, "%s connection failed; check pi-voice-%s.service",
            engine_name(op->engine), op->engine);
        set_backend(op->audio, op->engine, "error", result->error);
        free(response.body);
        return -1;
    }
    if (response.status >= 400) {
        snprintf(result->error, sizeof result->error, "%s request failed (HTTP %d); check pi-voice-%s.service",
            engine_name(op->engine), response.status, op->engine);
        set_backend(op->audio, op->engine, "error", result->error);
        free(response.body);
        return -1;
    }
    if (response.body_len > op->maximum) {
        snprintf(result->error, sizeof result->error, "%s response exceeds the size limit", engine_name(op->engine));
        set_backend(op->audio, op->engine, "error", result->error);
        free(response.body);
        return -1;
    }
    set_backend(op->audio, op->engine, "ready", NULL);
    if (strcmp(op->engine, "stt") == 0) {
        json_value *root = json_parse(response.body, response.body_len);
        free(response.body);
        if (!root) {
            set_err(result->error, sizeof result->error, "Whisper returned invalid JSON");
            return -1;
        }
        json_value *text = root->type == JSON_OBJECT ? json_get(root, "text") : NULL;
        if (!text || text->type != JSON_STRING) {
            json_free(root);
            set_err(result->error, sizeof result->error, "Whisper returned no transcription text");
            return -1;
        }
        result->text = strdup(text->string ? text->string : "");
        json_free(root);
        return result->text ? 0 : -1;
    }
    int rc = parse_wav_result(response.body, response.body_len, 0, result);
    free(response.body);
    return rc;
}

static int transcribe_deepgram(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, char *out, size_t out_cap, char *err, size_t err_cap) {
    char *key = copy_key();
    if (!key) {
        set_err(err, err_cap, "Deepgram API key is not installed; stay on Whisper");
        return -1;
    }
    unsigned char *payload = NULL;
    size_t payload_len = 0;
    if (read_file(path, &payload, &payload_len) != 0) {
        free(key);
        set_err(err, err_cap, "Could not read dictation audio");
        return -1;
    }
    char *url = strdup("https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true&punctuate=true&mip_opt_out=true");
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
    http_op *op = calloc(1, sizeof *op);
    op->audio = audio;
    op->url = url;
    op->content_type = strdup("audio/wav");
    op->authorization = malloc(strlen(key) + 8);
    sprintf(op->authorization, "Token %s", key);
    free(key);
    op->body = payload;
    op->body_len = payload_len;
    op->timeout_ms = 90000;
    op->maximum = LISTEN_MAX;
    op->deepgram_listen = 1;
    op_result result = {0};
    int rc = run_cancellable(audio, &audio->recognition_mu, NULL, cancelled, http_op_run, op, free_http_op,
        on_drained, drain_user, &result, err, err_cap);
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

static int transcribe_whisper(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, char *out, size_t out_cap, char *err, size_t err_cap) {
    if (!audio->stt_url) { set_err(err, err_cap, "Whisper request failed (HTTP 0); check pi-voice-stt.service"); return -1; }
    unsigned char *file = NULL;
    size_t file_len = 0;
    if (read_file(path, &file, &file_len) != 0) {
        set_err(err, err_cap, "Could not read dictation audio");
        return -1;
    }
    unsigned char random[16];
    FILE *entropy = fopen("/dev/urandom", "rb");
    if (!entropy || fread(random, 1, sizeof random, entropy) != sizeof random) {
        for (size_t i = 0; i < sizeof random; i++) random[i] = (unsigned char)(mono_now() * 1000 + i);
    }
    if (entropy) fclose(entropy);
    char boundary[33];
    for (int i = 0; i < 16; i++) sprintf(boundary + i * 2, "%02x", random[i]);
    char *fields = NULL;
    size_t flen = 0, fcap = 0;
    const char *names[] = {"response_format", "language", "temperature"};
    const char *values[] = {"json", audio->stt_language ? audio->stt_language : "en", "0"};
    for (int i = 0; i < 3; i++) {
        char header[256];
        snprintf(header, sizeof header, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
            boundary, names[i], values[i]);
        append_str(&fields, &flen, &fcap, header);
    }
    if (audio->stt_prompt) {
        char *header = malloc(strlen(boundary) + strlen(audio->stt_prompt) + 80);
        sprintf(header, "--%s\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n%s\r\n", boundary, audio->stt_prompt);
        append_str(&fields, &flen, &fcap, header);
        free(header);
    }
    char file_header[256];
    snprintf(file_header, sizeof file_header,
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"dictation.wav\"\r\nContent-Type: audio/wav\r\n\r\n",
        boundary);
    append_str(&fields, &flen, &fcap, file_header);
    char tail[80];
    snprintf(tail, sizeof tail, "\r\n--%s--\r\n", boundary);
    size_t total = flen + file_len + strlen(tail);
    unsigned char *body = malloc(total);
    memcpy(body, fields, flen);
    memcpy(body + flen, file, file_len);
    memcpy(body + flen + file_len, tail, strlen(tail));
    free(fields);
    free(file);
    http_op *op = calloc(1, sizeof *op);
    op->audio = audio;
    op->url = strdup(audio->stt_url);
    op->content_type = malloc(strlen(boundary) + 48);
    sprintf(op->content_type, "multipart/form-data; boundary=%s", boundary);
    op->body = body;
    op->body_len = total;
    op->timeout_ms = 90000;
    op->maximum = WHISPER_MAX;
    op->engine = strdup("stt");
    op_result result = {0};
    int rc = run_cancellable(audio, &audio->recognition_mu, "stt", cancelled, http_op_run, op, free_http_op,
        on_drained, drain_user, &result, err, err_cap);
    if (rc != 0) {
        if (rc > 0 && out && out_cap) out[0] = 0;
        return rc < 0 ? -1 : 0;
    }
    if (!out || strlen(result.text) + 1 > out_cap) {
        free_result(&result);
        set_err(err, err_cap, "transcription exceeds the buffer");
        return -1;
    }
    memcpy(out, result.text, strlen(result.text) + 1);
    free_result(&result);
    return 0;
}

int audio_transcribe(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, char *out, size_t out_cap, char *err, size_t err_cap) {
    if (out && out_cap) out[0] = 0;
    if (strcmp(audio->stt_backend, "deepgram") == 0)
        return transcribe_deepgram(audio, path, cancelled, on_drained, drain_user, out, out_cap, err, err_cap);
    int ready = audio_wait_ready(audio, "stt", cancelled, err, err_cap);
    if (ready <= 0) return ready < 0 ? -1 : 0;
    return transcribe_whisper(audio, path, cancelled, on_drained, drain_user, out, out_cap, err, err_cap);
}

static int word_count(const char *text) {
    int words = 0, in = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        if (isspace(*p)) in = 0;
        else if (!in) { in = 1; words++; }
    }
    return words;
}

static void free_choice(voice_choice *choice) { free_pairs(choice->pairs, choice->npairs); }

static int choose_voice(audio *audio, const char *text, voice_choice *choice, char *err, size_t err_cap) {
    memset(choice, 0, sizeof *choice);
    pthread_mutex_lock(&audio->mu);
    if (strcmp(audio->speech_backend, "deepgram") == 0) {
        choice->deepgram = 1;
        snprintf(choice->model, sizeof choice->model, "aura-2-%s-en", audio->deepgram_voice);
        pthread_mutex_unlock(&audio->mu);
        return 0;
    }
    int index = local_index(audio, audio->selected_voice);
    if (index < 0) {
        pthread_mutex_unlock(&audio->mu);
        set_err(err, err_cap, "That speech backend is not available");
        return -1;
    }
    snprintf(choice->tts_model, sizeof choice->tts_model, "%s", audio->tts_model ? audio->tts_model : "pi-voice");
    if (strcmp(audio->selected_voice, "samantha") == 0 && word_count(text) > 50) {
        choice->pairs = copy_pairs(audio->long_voice, audio->long_voice_count);
        choice->npairs = audio->long_voice_count;
    } else {
        choice->pairs = copy_pairs(audio->voices[index].options, audio->voices[index].option_count);
        choice->npairs = audio->voices[index].option_count;
    }
    pthread_mutex_unlock(&audio->mu);
    return 0;
}

static int build_tts_body(const voice_choice *choice, const char *chunk, unsigned char **body, size_t *len) {
    char *buf = NULL;
    size_t n = 0, cap = 0;
    if (append_str(&buf, &n, &cap, "{\"model\":") || append_json_string(&buf, &n, &cap, choice->tts_model)
        || append_str(&buf, &n, &cap, ",\"input\":") || append_json_string(&buf, &n, &cap, chunk)
        || append_str(&buf, &n, &cap, ",\"language\":") || append_json_string(&buf, &n, &cap, "English"))
        return -1;
    for (size_t i = 0; i < choice->npairs; i++) {
        if (append_str(&buf, &n, &cap, ",") || append_json_string(&buf, &n, &cap, choice->pairs[i].key)
            || append_str(&buf, &n, &cap, ":") || append_json_string(&buf, &n, &cap, choice->pairs[i].value))
            return -1;
    }
    if (append_str(&buf, &n, &cap, "}")) return -1;
    *body = (unsigned char *)buf;
    *len = n;
    return 0;
}

static int synthesize(audio *audio, const char *chunk, const voice_choice *choice, atomic_int *cancelled, op_result *result, char *err, size_t err_cap) {
    http_op *op = calloc(1, sizeof *op);
    if (!op) { set_err(err, err_cap, "Out of memory"); return -1; }
    op->audio = audio;
    if (choice->deepgram) {
        char *key = copy_key();
        if (!key) {
            free(op);
            set_err(err, err_cap, "Deepgram API key is not installed; use local speech");
            return -1;
        }
        op->url = strdup("https://api.deepgram.com/v1/speak");
        query_append(&op->url, "model", choice->model);
        query_append(&op->url, "encoding", "linear16");
        query_append(&op->url, "container", "wav");
        query_append(&op->url, "sample_rate", "24000");
        query_append(&op->url, "mip_opt_out", "true");
        op->content_type = strdup("application/json");
        op->authorization = malloc(strlen(key) + 8);
        sprintf(op->authorization, "Token %s", key);
        free(key);
        char *text = NULL;
        size_t n = 0, cap = 0;
        append_str(&text, &n, &cap, "{\"text\":");
        append_json_string(&text, &n, &cap, chunk);
        append_str(&text, &n, &cap, "}");
        op->body = (unsigned char *)text;
        op->body_len = n;
        op->timeout_ms = 60000;
        op->maximum = SPEAK_MAX;
        op->deepgram_speak = 1;
        return run_cancellable(audio, &audio->synthesis_mu, NULL, cancelled, http_op_run, op, free_http_op,
            NULL, NULL, result, err, err_cap);
    }
    if (!audio->tts_url) {
        free(op);
        set_err(err, err_cap, "Samantha TTS request failed (HTTP 0); check pi-voice-tts.service");
        return -1;
    }
    op->url = strdup(audio->tts_url);
    op->content_type = strdup("application/json");
    op->engine = strdup("tts");
    op->timeout_ms = 90000;
    op->maximum = TTS_MAX;
    if (build_tts_body(choice, chunk, &op->body, &op->body_len) != 0) {
        free_http_op(op);
        set_err(err, err_cap, "Out of memory");
        return -1;
    }
    return run_cancellable(audio, &audio->synthesis_mu, "tts", cancelled, http_op_run, op, free_http_op,
        NULL, NULL, result, err, err_cap);
}

typedef struct real_player {
    audio_player base;
    pid_t pid;
    int stdin_fd;
    int reaped;
    int code;
} real_player;

static real_player *real_of(audio_player *player) { return player->user; }

static int real_write(audio_player *player, const void *data, size_t len) {
    real_player *real = real_of(player);
    return write_all(real->stdin_fd, data, len) == 0 ? (int)len : -1;
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
    real_close(player);
    free(player->user);
}

static audio_player *default_popen(const audio_spawn *spawn) {
    int pipefd[2] = {-1, -1};
    if (spawn->pipe_stdin && pipe(pipefd) != 0) return NULL;
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
    int old_flags = -1;
    if (spawn->pass_fd >= 0) {
        old_flags = fcntl(spawn->pass_fd, F_GETFD);
        if (old_flags >= 0) fcntl(spawn->pass_fd, F_SETFD, old_flags & ~FD_CLOEXEC);
    }
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, spawn->argv[0], &actions, NULL, (char *const *)spawn->argv, environ);
    if (spawn->pass_fd >= 0 && old_flags >= 0) fcntl(spawn->pass_fd, F_SETFD, old_flags);
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

static int play_fd(audio *audio, int fd, atomic_int *cancelled, char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    const char *argv[] = {"pw-play", path, NULL};
    audio_spawn spawn = {.argv = argv, .argc = 2, .pass_fd = fd, .pipe_stdin = 0};
    pthread_mutex_lock(&audio->mu);
    int cancel = cancelled && atomic_load(cancelled);
    pthread_mutex_unlock(&audio->mu);
    if (cancel) return 0;
    audio_player *player = open_player(audio, &spawn);
    if (!player) { set_err(err, err_cap, "Speech playback failed"); return -1; }
    pthread_mutex_lock(&audio->mu);
    if (cancelled && atomic_load(cancelled)) {
        pthread_mutex_unlock(&audio->mu);
        if (player->destroy) player->destroy(player);
        return 0;
    }
    audio->player = player;
    audio_audible_fn fn = audio->audible;
    void *user = audio->audible_user;
    pthread_mutex_unlock(&audio->mu);
    if (fn) fn(user);
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
    pthread_mutex_lock(&audio->mu);
    int cancel = cancelled && atomic_load(cancelled);
    pthread_mutex_unlock(&audio->mu);
    if (cancel) { free_result(&first); return 0; }
    audio_player *player = open_player(audio, &spawn);
    if (!player) { free_result(&first); set_err(err, err_cap, "Speech playback failed"); return -1; }
    pthread_mutex_lock(&audio->mu);
    audio->player = player;
    audio_audible_fn fn = audio->audible;
    void *user = audio->audible_user;
    pthread_mutex_unlock(&audio->mu);
    if (fn) fn(user);
    unsigned char *frames = first.frames;
    size_t frame_len = first.frame_len;
    int ch = first.channels, width = first.width, sample_rate = first.rate;
    first.frames = NULL;
    prefetch pending = {0};
    int failed = 0;
    for (size_t i = 0; i < count; i++) {
        if (cancelled && atomic_load(cancelled)) break;
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
                if (!(cancelled && atomic_load(cancelled))) {
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
            if (!(cancelled && atomic_load(cancelled))) {
                set_err(err, err_cap, "Speech playback stopped unexpectedly");
                failed = 1;
            }
            break;
        }
    }
    free(frames);
    if (!failed && !(cancelled && atomic_load(cancelled))) {
        if (player->close_stdin) player->close_stdin(player);
        player->stdin_closed = 1;
        int code = player->wait ? player->wait(player, -1) : 0;
        if (code && !(cancelled && atomic_load(cancelled))) {
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
    if (!choice.deepgram) {
        int ready = audio_wait_ready(audio, "tts", cancelled, err, err_cap);
        if (ready <= 0) {
            free_chunks(chunks, count);
            free_choice(&choice);
            return ready < 0 ? -1 : 0;
        }
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

int audio_call(audio *audio, const char *engine, atomic_int *cancelled,
    int (*op)(void *user, char *err, size_t err_cap), void *user, char *err, size_t err_cap) {
    call_op *call = calloc(1, sizeof *call);
    if (!call) { set_err(err, err_cap, "Out of memory"); return -1; }
    call->op = op;
    call->user = user;
    op_result result = {0};
    int rc = run_cancellable(audio, &audio->call_mu, engine, cancelled, call_op_run, call, free_call_op,
        NULL, NULL, &result, err, err_cap);
    free_result(&result);
    return rc;
}
