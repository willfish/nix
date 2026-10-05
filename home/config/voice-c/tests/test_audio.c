#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "capture.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static const char *test_name;
static char current_key[96];

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, test_name, #cond); \
        failures++; \
    } \
} while (0)

static void fail_msg(const char *msg) {
    fprintf(stderr, "FAIL %s:%d %s: %s\n", __FILE__, __LINE__, test_name, msg);
    failures++;
}

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static int join_ms(pthread_t thread, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_timedjoin_np(thread, NULL, &ts);
}

typedef struct gate {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int set;
} gate;

static void gate_init(gate *g) {
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
    g->set = 0;
}

static void gate_destroy(gate *g) {
    pthread_cond_destroy(&g->cv);
    pthread_mutex_destroy(&g->mu);
}

static void gate_set(gate *g) {
    pthread_mutex_lock(&g->mu);
    g->set = 1;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

static int gate_wait(gate *g, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g->mu);
    while (!g->set) {
        if (pthread_cond_timedwait(&g->cv, &g->mu, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&g->mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g->mu);
    return 1;
}

static void use_key(const char *key) {
    if (key && key[0]) {
        setenv("DEEPGRAM_API_KEY", key, 1);
        snprintf(current_key, sizeof current_key, "%s", key);
    } else {
        unsetenv("DEEPGRAM_API_KEY");
        current_key[0] = 0;
    }
}

static int contains_key(const char *text) {
    return current_key[0] && text && strstr(text, current_key) != NULL;
}

static char *make_tmp(void) {
    char buf[] = "/tmp/voice-audio-XXXXXX";
    if (!mkdtemp(buf)) return NULL;
    return strdup(buf);
}

static void rm_rf(const char *dir) {
    if (!dir) return;
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    int rc = system(cmd);
    (void)rc;
}

static int write_file(const char *path, const void *data, size_t len) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    int ok = fwrite(data, 1, len, file) == len;
    fclose(file);
    return ok ? 0 : -1;
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, file);
    fclose(file);
    buf[n] = 0;
    return strdup(buf);
}

static int dir_entries(const char *dir) {
    DIR *handle = opendir(dir);
    if (!handle) return -1;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(handle))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        n++;
    }
    closedir(handle);
    return n;
}

static int count_suffix(const char *dir, const char *suffix) {
    DIR *handle = opendir(dir);
    if (!handle) return -1;
    int n = 0;
    size_t slen = strlen(suffix);
    struct dirent *ent;
    while ((ent = readdir(handle))) {
        size_t len = strlen(ent->d_name);
        if (len >= slen && strcmp(ent->d_name + len - slen, suffix) == 0) n++;
    }
    closedir(handle);
    return n;
}

static unsigned char *build_wav(int channels, int rate, int width, const unsigned char *frames, size_t nbytes, size_t *out_len) {
    size_t len = 44 + nbytes;
    unsigned char *data = calloc(1, len ? len : 1);
    if (!data) return NULL;
    memcpy(data, "RIFF", 4);
    uint32_t chunk = (uint32_t)(36 + nbytes);
    data[4] = chunk & 255;
    data[5] = (chunk >> 8) & 255;
    data[6] = (chunk >> 16) & 255;
    data[7] = (chunk >> 24) & 255;
    memcpy(data + 8, "WAVE", 4);
    memcpy(data + 12, "fmt ", 4);
    data[16] = 16;
    data[20] = 1;
    data[22] = (unsigned char)(channels & 255);
    data[23] = (unsigned char)((channels >> 8) & 255);
    data[24] = (unsigned char)(rate & 255);
    data[25] = (unsigned char)((rate >> 8) & 255);
    data[26] = (unsigned char)((rate >> 16) & 255);
    data[27] = (unsigned char)((rate >> 24) & 255);
    uint32_t byte_rate = (uint32_t)(rate * channels * width);
    data[28] = byte_rate & 255;
    data[29] = (byte_rate >> 8) & 255;
    data[30] = (byte_rate >> 16) & 255;
    data[31] = (byte_rate >> 24) & 255;
    uint16_t block = (uint16_t)(channels * width);
    data[32] = block & 255;
    data[33] = (block >> 8) & 255;
    data[34] = (unsigned char)((width * 8) & 255);
    data[35] = (unsigned char)(((width * 8) >> 8) & 255);
    memcpy(data + 36, "data", 4);
    data[40] = (unsigned char)(nbytes & 255);
    data[41] = (unsigned char)((nbytes >> 8) & 255);
    data[42] = (unsigned char)((nbytes >> 16) & 255);
    data[43] = (unsigned char)((nbytes >> 24) & 255);
    if (nbytes) memcpy(data + 44, frames, nbytes);
    *out_len = len;
    return data;
}

static unsigned char *silence_wav(size_t *len) {
    unsigned char frames[48] = {0};
    return build_wav(1, 24000, 2, frames, sizeof frames, len);
}

static int words_in(const char *text) {
    int count = 0, in = 0;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        if (*p == ' ' || *p == '\n' || *p == '\t') in = 0;
        else if (!in) { in = 1; count++; }
    }
    return count;
}

static int json_field(const char *body, size_t len, const char *key, char *out, size_t cap) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const unsigned char *found = memmem(body, len, pat, strlen(pat));
    if (!found) return 0;
    const char *p = (const char *)found + strlen(pat);
    const char *end = body + len;
    if (p >= end || *p != '"') return 0;
    p++;
    size_t n = 0;
    while (p < end && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return 1;
}

static int field_absent(const char *body, size_t len, const char *key) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    return memmem(body, len, pat, strlen(pat)) == NULL;
}

typedef struct req_log {
    char url[768];
    char content_type[160];
    unsigned char *body;
    size_t body_len;
    int auth_present;
    int auth_matches;
    int key_in_url;
    int key_in_body;
} req_log;

static req_log requests[32];
static int request_count;
static pthread_mutex_t req_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int http_calls;
static int (*http_handle)(const audio_http_request *, audio_http_response *, void *);

static void clear_requests(void) {
    pthread_mutex_lock(&req_mu);
    for (int i = 0; i < request_count; i++) free(requests[i].body);
    memset(requests, 0, sizeof requests);
    request_count = 0;
    atomic_store(&http_calls, 0);
    pthread_mutex_unlock(&req_mu);
}

static void respond_text(audio_http_response *resp, int status, const char *text) {
    resp->status = status;
    resp->body = (unsigned char *)strdup(text ? text : "");
    resp->body_len = resp->body ? strlen(text ? text : "") : 0;
}

static void respond_bin(audio_http_response *resp, int status, const unsigned char *data, size_t len) {
    resp->status = status;
    resp->body = malloc(len ? len : 1);
    if (resp->body && len) memcpy(resp->body, data, len);
    resp->body_len = resp->body ? len : 0;
}

static int record_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)user;
    memset(response, 0, sizeof *response);
    int call = atomic_fetch_add(&http_calls, 1) + 1;
    pthread_mutex_lock(&req_mu);
    if (request_count < 32) {
        req_log *log = &requests[request_count++];
        snprintf(log->url, sizeof log->url, "%s", request->url ? request->url : "");
        snprintf(log->content_type, sizeof log->content_type, "%s", request->content_type ? request->content_type : "");
        log->auth_present = request->authorization && request->authorization[0];
        if (current_key[0] && request->authorization) {
            char expect[128];
            snprintf(expect, sizeof expect, "Token %s", current_key);
            log->auth_matches = strcmp(request->authorization, expect) == 0;
        }
        log->key_in_url = contains_key(request->url);
        if (request->body && request->body_len) {
            log->body = malloc(request->body_len);
            if (log->body) memcpy(log->body, request->body, request->body_len);
            log->body_len = request->body_len;
            if (current_key[0] && memmem(request->body, request->body_len, current_key, strlen(current_key)))
                log->key_in_body = 1;
        }
    }
    pthread_mutex_unlock(&req_mu);
    if (!http_handle) {
        response->transport_error = 1;
        fail_msg("unexpected HTTP");
        return 0;
    }
    return http_handle(request, response, (void *)(intptr_t)call);
}

typedef struct fake_player {
    audio_player base;
    int poll_code;
    int wait_code;
    void (*on_write)(struct fake_player *, const void *, size_t);
    void *user;
} fake_player;

typedef struct play_stats {
    int opened;
    int terminate;
    int close_calls;
    int wait_calls;
    int last_wait_ms;
    int kill_calls;
    int pipe_stdin;
    int pass_fd;
    int http_at_open;
    int argc;
    char argv[12][80];
    unsigned char *fd_bytes;
    size_t fd_len;
    unsigned char *writes[8];
    size_t write_len[8];
    int nwrites;
    int channels;
    int width;
    int rate;
} play_stats;

static play_stats stats;
static pthread_mutex_t stats_mu = PTHREAD_MUTEX_INITIALIZER;
static fake_player *live_player;
static void (*write_hook)(fake_player *, const void *, size_t);

static void reset_stats(void) {
    pthread_mutex_lock(&stats_mu);
    free(stats.fd_bytes);
    for (int i = 0; i < stats.nwrites; i++) free(stats.writes[i]);
    memset(&stats, 0, sizeof stats);
    pthread_mutex_unlock(&stats_mu);
    live_player = NULL;
    write_hook = NULL;
}

static int fake_write(audio_player *player, const void *data, size_t len) {
    fake_player *fake = player->user;
    pthread_mutex_lock(&stats_mu);
    if (stats.nwrites < 8) {
        stats.writes[stats.nwrites] = malloc(len ? len : 1);
        if (stats.writes[stats.nwrites] && len) memcpy(stats.writes[stats.nwrites], data, len);
        stats.write_len[stats.nwrites] = len;
        stats.nwrites++;
    }
    pthread_mutex_unlock(&stats_mu);
    if (write_hook) write_hook(fake, data, len);
    return (int)len;
}

static void fake_close(audio_player *player) {
    player->stdin_closed = 1;
    pthread_mutex_lock(&stats_mu);
    stats.close_calls++;
    pthread_mutex_unlock(&stats_mu);
}

static int block_until_stop;
static int spawn_wait_code;

static int fake_wait(audio_player *player, int timeout_ms) {
    fake_player *fake = player->user;
    pthread_mutex_lock(&stats_mu);
    stats.wait_calls++;
    stats.last_wait_ms = timeout_ms;
    pthread_mutex_unlock(&stats_mu);
    if (block_until_stop && timeout_ms < 0) {
        double start = mono_now();
        while (fake->poll_code < 0 && mono_now() - start < 1.5) sleep_ms(10);
        return fake->poll_code;
    }
    /* A completed wait reaps the player, matching pw-play. Timeout leaves it running. */
    if (fake->wait_code >= 0) fake->poll_code = fake->wait_code;
    return fake->wait_code;
}

static int fake_poll(audio_player *player) {
    return ((fake_player *)player->user)->poll_code;
}

static void fake_terminate(audio_player *player) {
    fake_player *fake = player->user;
    pthread_mutex_lock(&stats_mu);
    stats.terminate++;
    pthread_mutex_unlock(&stats_mu);
    fake->poll_code = 0;
}

static void fake_kill(audio_player *player) {
    (void)player;
    pthread_mutex_lock(&stats_mu);
    stats.kill_calls++;
    pthread_mutex_unlock(&stats_mu);
}

static void fake_destroy(audio_player *player) {
    free(player->user);
    if (live_player && &live_player->base == player) live_player = NULL;
}

static int read_fd_all(int fd, unsigned char **out, size_t *len) {
    off_t cur = lseek(fd, 0, SEEK_CUR);
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) return -1;
    unsigned char *buf = malloc((size_t)end + 1);
    if (!buf) return -1;
    lseek(fd, 0, SEEK_SET);
    size_t got = 0;
    while (got < (size_t)end) {
        ssize_t n = read(fd, buf + got, (size_t)end - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (cur >= 0) lseek(fd, cur, SEEK_SET);
    *out = buf;
    *len = got;
    return 0;
}

static void note_wav(const unsigned char *data, size_t len) {
    if (len < 44 || memcmp(data, "RIFF", 4) != 0) return;
    pthread_mutex_lock(&stats_mu);
    stats.channels = data[22] | (data[23] << 8);
    stats.rate = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
    stats.width = (data[34] | (data[35] << 8)) / 8;
    pthread_mutex_unlock(&stats_mu);
}

static audio_player *record_popen(const audio_spawn *spawn, void *user) {
    (void)user;
    pthread_mutex_lock(&stats_mu);
    stats.opened++;
    stats.http_at_open = atomic_load(&http_calls);
    stats.pipe_stdin = spawn->pipe_stdin;
    stats.pass_fd = spawn->pass_fd;
    stats.argc = spawn->argc;
    for (int i = 0; i < spawn->argc && i < 12; i++)
        snprintf(stats.argv[i], sizeof stats.argv[i], "%s", spawn->argv[i] ? spawn->argv[i] : "");
    pthread_mutex_unlock(&stats_mu);
    if (spawn->pass_fd >= 0) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        if (read_fd_all(spawn->pass_fd, &bytes, &len) == 0) {
            note_wav(bytes, len);
            pthread_mutex_lock(&stats_mu);
            free(stats.fd_bytes);
            stats.fd_bytes = bytes;
            stats.fd_len = len;
            pthread_mutex_unlock(&stats_mu);
        }
    }
    fake_player *fake = calloc(1, sizeof *fake);
    fake->poll_code = -1;
    fake->wait_code = spawn_wait_code;
    fake->base.write = fake_write;
    fake->base.close_stdin = fake_close;
    fake->base.wait = fake_wait;
    fake->base.poll = fake_poll;
    fake->base.terminate = fake_terminate;
    fake->base.kill = fake_kill;
    fake->base.destroy = fake_destroy;
    fake->base.user = fake;
    live_player = fake;
    return &fake->base;
}

static int argv_has(const char *text) {
    for (int i = 0; i < stats.argc; i++) {
        if (strcmp(stats.argv[i], text) == 0) return 1;
    }
    return 0;
}

static atomic_int acquire_count;
static atomic_int lease_released;
static char acquired_engine[32];

static int lease_wait(audio_lease *lease, int timeout_ms, char *err, size_t err_cap) {
    (void)lease; (void)timeout_ms; (void)err; (void)err_cap;
    return 0;
}

static int lease_ready(audio_lease *lease) {
    (void)lease;
    return 1;
}

static void lease_release(audio_lease *lease) {
    atomic_fetch_add(&lease_released, 1);
    free(lease->user);
}

static audio_lease *acquire_engine(const char *engine, void *user) {
    (void)user;
    atomic_fetch_add(&acquire_count, 1);
    snprintf(acquired_engine, sizeof acquired_engine, "%s", engine ? engine : "");
    audio_lease *lease = calloc(1, sizeof *lease);
    lease->wait_ms = lease_wait;
    lease->ready_done = lease_ready;
    lease->release = lease_release;
    lease->user = lease;
    return lease;
}

static void attach_audio(audio *audio) {
    audio_set_http(audio, record_http, NULL);
    audio_set_popen(audio, record_popen, NULL);
    audio_set_engines(audio, acquire_engine, NULL);
}

static char *saved_key;

static void stash_env_key(void) {
    const char *key = getenv("DEEPGRAM_API_KEY");
    if (key && key[0]) saved_key = strdup(key);
    unsetenv("DEEPGRAM_API_KEY");
    current_key[0] = 0;
}

static void restore_env_key(void) {
    if (!saved_key) return;
    setenv("DEEPGRAM_API_KEY", saved_key, 1);
    free(saved_key);
    saved_key = NULL;
}

static int file_contains_key(const char *path) {
    char *text = read_text(path);
    if (!text) return 0;
    int found = contains_key(text);
    free(text);
    return found;
}

static int tree_contains_key(const char *dir) {
    DIR *handle = opendir(dir);
    if (!handle) return 0;
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(handle))) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        if (file_contains_key(path)) found = 1;
    }
    closedir(handle);
    return found;
}

static void expect_no_key_leak(const char *err, const char *dir, const char *stderr_path) {
    if (contains_key(err)) fail_msg("API key appeared in an error");
    if (dir && tree_contains_key(dir)) fail_msg("API key was written into the runtime directory");
    if (stderr_path && file_contains_key(stderr_path)) fail_msg("API key was written to stderr");
    pthread_mutex_lock(&req_mu);
    for (int i = 0; i < request_count; i++) {
        if (requests[i].key_in_url) fail_msg("API key appeared in a request URL");
        if (requests[i].key_in_body) fail_msg("API key appeared in a request body");
    }
    pthread_mutex_unlock(&req_mu);
}

static int capture_stderr(const char *path) {
    fflush(stderr);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return -1;
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    return saved;
}

static void restore_stderr(int saved) {
    fflush(stderr);
    if (saved >= 0) {
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
}

static char *repeat(const char *piece, int count) {
    size_t n = strlen(piece);
    char *out = malloc(n * (size_t)count + 1);
    if (!out) return NULL;
    for (int i = 0; i < count; i++) memcpy(out + (size_t)i * n, piece, n);
    out[n * (size_t)count] = 0;
    return out;
}

static void free_chunks(char **chunks, size_t count) {
    for (size_t i = 0; i < count; i++) free(chunks[i]);
    free(chunks);
}

static void test_chunks(void) {
    test_name = "chunks";
    const char *first = "The voice is now ready to use.";
    const char *second = "The recorder waits for your microphone before it starts.";
    const char *third = "Dictation stays in your selected terminal until you send it.";
    const char *fourth = "You can cancel recording or playback from the tray.";
    char text[512];
    snprintf(text, sizeof text, "%s %s %s %s", first, second, third, fourth);
    char **chunks = NULL;
    size_t count = 0;
    CHECK(audio_speech_chunks(text, &chunks, &count) == 0);
    CHECK(count == 2);
    char want0[256], want1[256];
    snprintf(want0, sizeof want0, "%s %s", first, second);
    snprintf(want1, sizeof want1, "%s %s", third, fourth);
    if (count == 2) {
        CHECK(strcmp(chunks[0], want0) == 0);
        CHECK(strcmp(chunks[1], want1) == 0);
    }
    free_chunks(chunks, count);

    char *sentence = repeat("captures your voice ", 8);
    char long_sentence[512];
    snprintf(long_sentence, sizeof long_sentence, "The recorder %sreliably.", sentence);
    free(sentence);
    CHECK(strlen(long_sentence) > 120);
    chunks = NULL;
    count = 0;
    CHECK(audio_speech_chunks(long_sentence, &chunks, &count) == 0);
    CHECK(count == 1);
    if (count == 1) CHECK(strcmp(chunks[0], long_sentence) == 0);
    free_chunks(chunks, count);

    char *clause_body = repeat("all of your words ", 4);
    char clause[256];
    snprintf(clause, sizeof clause, "These changes keep %ssafe;", clause_body);
    free(clause_body);
    char *tail = repeat("recognition ", 30);
    char clause_text[1024];
    snprintf(clause_text, sizeof clause_text, "%s %s", clause, tail);
    free(tail);
    chunks = NULL;
    count = 0;
    CHECK(audio_speech_chunks(clause_text, &chunks, &count) == 0);
    CHECK(count > 0);
    if (count) CHECK(strcmp(chunks[0], clause) == 0);
    free_chunks(chunks, count);

    char word[812];
    memset(word, 'x', 811);
    word[811] = 0;
    chunks = NULL;
    count = 0;
    CHECK(audio_speech_chunks(word, &chunks, &count) == 0);
    char joined[812] = "";
    int bounded = count > 0;
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(chunks[i]);
        if (n == 0 || n > 260) bounded = 0;
        strcat(joined, chunks[i]);
    }
    CHECK(bounded);
    CHECK(count > 0 && strlen(chunks[0]) == 120);
    CHECK(strcmp(joined, word) == 0);
    free_chunks(chunks, count);
}

typedef struct hold_ctx {
    gate started;
    gate drain;
    atomic_int *cancelled;
    int rc;
    audio *audio;
} hold_ctx;

static int gpu_op(void *user, char *err, size_t err_cap) {
    hold_ctx *ctx = user;
    (void)err; (void)err_cap;
    gate_set(&ctx->started);
    if (!gate_wait(&ctx->drain, 2000)) return -1;
    return 0;
}

static void *hold_main(void *arg) {
    hold_ctx *ctx = arg;
    char err[128];
    ctx->rc = audio_call(ctx->audio, "stt", ctx->cancelled, gpu_op, ctx, err, sizeof err);
    return NULL;
}

static void test_cancelled_request_holds_engine(void) {
    test_name = "cancelled request holds engine";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    atomic_store(&acquire_count, 0);
    atomic_store(&lease_released, 0);
    acquired_engine[0] = 0;
    hold_ctx ctx = {.audio = audio};
    gate_init(&ctx.started);
    gate_init(&ctx.drain);
    atomic_int cancelled = 0;
    ctx.cancelled = &cancelled;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, hold_main, &ctx) == 0);
    CHECK(gate_wait(&ctx.started, 1000));
    atomic_store(&cancelled, 1);
    int joined = join_ms(thread, 1000);
    CHECK(joined == 0);
    CHECK(ctx.rc == 1);
    CHECK(atomic_load(&lease_released) == 0);
    gate_set(&ctx.drain);
    int released = 0;
    for (int i = 0; i < 100; i++) {
        if (atomic_load(&lease_released) == 1) { released = 1; break; }
        sleep_ms(10);
    }
    CHECK(released);
    CHECK(atomic_load(&acquire_count) == 1);
    CHECK(strcmp(acquired_engine, "stt") == 0);
    if (joined != 0) {
        gate_set(&ctx.drain);
        join_ms(thread, 1000);
    }
    audio_free(audio);
    gate_destroy(&ctx.started);
    gate_destroy(&ctx.drain);
    rm_rf(dir);
    free(dir);
}

static char captured_target[128];
static char captured_path[512];
static int resolve_saw_preferred;

static int mic_resolve(void *user, const char *preferred, char *target, size_t target_cap, char *name, size_t name_cap) {
    (void)user;
    resolve_saw_preferred = preferred && strcmp(preferred, "usb.microphone") == 0;
    snprintf(target, target_cap, "%s", "usb.microphone");
    snprintf(name, name_cap, "%s", "Resolved microphone");
    return 0;
}

static void mic_status(void *user, char *name, size_t name_cap, char *target, size_t target_cap) {
    (void)user;
    snprintf(name, name_cap, "%s", "Test microphone");
    snprintf(target, target_cap, "%s", "usb.microphone");
}

static void *capture_fn(const char *path, const char *target, void *user) {
    (void)user;
    snprintf(captured_path, sizeof captured_path, "%s", path ? path : "");
    snprintf(captured_target, sizeof captured_target, "%s", target ? target : "");
    return (void *)(intptr_t)1;
}

static void test_capture_reports_name(void) {
    test_name = "capture resolves preferred device";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.preferred_microphone = "usb.microphone";
    audio *audio = audio_new(dir, &config);
    audio_set_microphone(audio, mic_status, mic_resolve, NULL);
    audio_set_capture(audio, capture_fn, NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/capture.wav", dir);
    resolve_saw_preferred = 0;
    void *handle = audio_start_capture(audio, path, NULL, 0);
    CHECK(handle == (void *)(intptr_t)1);
    CHECK(resolve_saw_preferred);
    CHECK(strcmp(captured_target, "usb.microphone") == 0);
    CHECK(strcmp(captured_path, path) == 0);
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.microphone_name, "Test microphone") == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static int error_http(const audio_http_request *request, audio_http_response *response, void *user) {
    int call = (int)(intptr_t)user;
    (void)request; (void)call;
    respond_text(response, 500, "failed");
    return 0;
}

static void test_recognition_failure_preserves_recording(void) {
    test_name = "recognition failure preserves recording";
    use_key(NULL);
    char *dir = make_tmp();
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    CHECK(write_file(path, wav, wav_len) == 0);
    audio_config config;
    audio_config_init(&config);
    config.stt_url = "http://whisper.test/inference";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    http_handle = error_http;
    char out[128], err[512];
    atomic_int cancelled = 0;
    int rc = audio_transcribe(audio, path, &cancelled, NULL, NULL, out, sizeof out, err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "Whisper") != NULL);
    CHECK(strstr(err, "HTTP 500") != NULL);
    CHECK(access(path, F_OK) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (size_t)st.st_size == wav_len);
    audio_free(audio);
    free(wav);
    rm_rf(dir);
    free(dir);
}

static int bad_json_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 200, "not-json");
    return 0;
}

static void test_recognition_rejects_invalid_json(void) {
    test_name = "recognition rejects invalid json";
    use_key(NULL);
    char *dir = make_tmp();
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    write_file(path, wav, wav_len);
    audio_config config;
    audio_config_init(&config);
    config.stt_url = "http://whisper.test/inference";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    http_handle = bad_json_http;
    char out[128], err[512];
    int rc = audio_transcribe(audio, path, NULL, NULL, NULL, out, sizeof out, err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "Whisper") != NULL);
    CHECK(strstr(err, "invalid JSON") != NULL);
    audio_free(audio);
    free(wav);
    rm_rf(dir);
    free(dir);
}

static int busy_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 503, "busy");
    return 0;
}

static void test_synthesis_failure_does_not_play(void) {
    test_name = "synthesis failure does not play";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    reset_stats();
    clear_requests();
    http_handle = busy_http;
    char err[512];
    atomic_int cancelled = 0;
    int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "Samantha") != NULL);
    CHECK(strstr(err, "HTTP 503") != NULL);
    CHECK(stats.opened == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static atomic_int health_checks;

static int loading_then_ready_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)user;
    atomic_fetch_add(&health_checks, 1);
    (void)request;
    if (atomic_load(&health_checks) == 1)
        respond_text(response, 200, "{\"data\":[{\"id\":\"pi-voice\",\"loaded\":false}]}");
    else
        respond_text(response, 200, "{\"data\":[{\"id\":\"pi-voice\",\"loaded\":true}]}");
    return 0;
}

static void test_tts_readiness_waits_for_model(void) {
    test_name = "tts readiness waits for model";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_health_url = "http://tts.test/health";
    config.readiness_timeout = 1;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    atomic_store(&health_checks, 0);
    http_handle = loading_then_ready_http;
    char err[512];
    int rc = audio_wait_ready(audio, "tts", NULL, err, sizeof err);
    CHECK(rc == 1);
    CHECK(atomic_load(&health_checks) >= 2);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static int missing_model_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 200, "{\"data\":[]}");
    return 0;
}

static void test_missing_tts_model(void) {
    test_name = "missing tts model";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_health_url = "http://tts.test/health";
    config.readiness_timeout = 1;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    http_handle = missing_model_http;
    char err[512];
    int rc = audio_wait_ready(audio, "tts", NULL, err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "pi-voice") != NULL);
    CHECK(strstr(err, "missing") != NULL);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static gate slow_entered;
static gate slow_release;

static int slow_health_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    gate_set(&slow_entered);
    gate_wait(&slow_release, 1000);
    respond_text(response, 200, "{\"status\":\"ok\"}");
    return 0;
}

static void test_status_does_not_wait_for_health(void) {
    test_name = "status does not wait for health";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.stt_health_url = "http://stt.test/health";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    gate_init(&slow_entered);
    gate_init(&slow_release);
    http_handle = slow_health_http;
    audio_report report;
    double start = mono_now();
    CHECK(audio_status(audio, &report) == 0);
    CHECK(mono_now() - start < 0.1);
    CHECK(gate_wait(&slow_entered, 1000));
    gate_set(&slow_release);
    int ready = 0;
    double deadline = mono_now() + 1;
    while (mono_now() < deadline) {
        audio_status(audio, &report);
        if (strcmp(report.stt_state, "ready") == 0) { ready = 1; break; }
        sleep_ms(10);
    }
    CHECK(ready);
    gate_set(&slow_release);
    audio_free(audio);
    gate_destroy(&slow_entered);
    gate_destroy(&slow_release);
    rm_rf(dir);
    free(dir);
}

static int startup_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)user;
    if (strstr(request->url, "/health")) {
        int n = atomic_fetch_add(&health_checks, 1) + 1;
        if (n == 1) respond_text(response, 503, "{\"status\":\"loading\"}");
        else respond_text(response, 200, "{\"status\":\"ok\"}");
        return 0;
    }
    respond_text(response, 200, "{\"text\":\"ready words\"}");
    return 0;
}

static void test_recognition_waits_for_startup(void) {
    test_name = "recognition waits for startup";
    use_key(NULL);
    char *dir = make_tmp();
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    write_file(path, wav, wav_len);
    audio_config config;
    audio_config_init(&config);
    config.stt_url = "http://stt.test/inference";
    config.stt_health_url = "http://stt.test/health";
    config.readiness_timeout = 1;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    atomic_store(&health_checks, 0);
    http_handle = startup_http;
    char out[128] = {0}, err[512] = {0};
    int rc = audio_transcribe(audio, path, NULL, NULL, NULL, out, sizeof out, err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(out, "ready words") == 0);
    CHECK(atomic_load(&health_checks) >= 2);
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.stt_state, "ready") == 0);
    audio_free(audio);
    free(wav);
    rm_rf(dir);
    free(dir);
}

static int always_loading_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 503, "{\"status\":\"loading\"}");
    return 0;
}

static void test_unavailable_model_is_bounded(void) {
    test_name = "unavailable model is bounded";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.stt_health_url = "http://stt.test/health";
    config.readiness_timeout = 0.15;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    http_handle = always_loading_http;
    char err[512] = {0};
    atomic_int cancelled = 0;
    double start = mono_now();
    int rc = audio_wait_ready(audio, "stt", &cancelled, err, sizeof err);
    CHECK(mono_now() - start < 0.6);
    CHECK(rc == -1);
    CHECK(strstr(err, "Whisper") != NULL);
    CHECK(strstr(err, "not ready") != NULL);
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.stt_state, "error") == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

typedef struct ready_ctx {
    audio *audio;
    atomic_int *cancelled;
    int rc;
    gate done;
} ready_ctx;

static void *ready_main(void *arg) {
    ready_ctx *ctx = arg;
    char err[256];
    ctx->rc = audio_wait_ready(ctx->audio, "tts", ctx->cancelled, err, sizeof err);
    gate_set(&ctx->done);
    return NULL;
}

static int entered_loading_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request;
    (void)user;
    gate_set(&slow_entered);
    respond_text(response, 503, "{\"status\":\"loading\"}");
    return 0;
}

static void test_cancel_interrupts_readiness(void) {
    test_name = "cancel interrupts readiness";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_health_url = "http://tts.test/health";
    config.readiness_timeout = 2;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    gate_init(&slow_entered);
    http_handle = entered_loading_http;
    atomic_int cancelled = 0;
    ready_ctx ctx = {.audio = audio, .cancelled = &cancelled};
    gate_init(&ctx.done);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, ready_main, &ctx) == 0);
    CHECK(gate_wait(&slow_entered, 1000));
    atomic_store(&cancelled, 1);
    CHECK(gate_wait(&ctx.done, 500));
    CHECK(ctx.rc == 0);
    join_ms(thread, 1000);
    audio_free(audio);
    gate_destroy(&slow_entered);
    gate_destroy(&ctx.done);
    rm_rf(dir);
    free(dir);
}

static int vocab_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 200, "{\"text\":\"Check Herdr and NixOS.\"}");
    return 0;
}

static void test_recognition_sends_vocabulary(void) {
    test_name = "recognition sends vocabulary";
    use_key(NULL);
    char *dir = make_tmp();
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    write_file(path, wav, wav_len);
    audio_config config;
    audio_config_init(&config);
    config.stt_url = "http://whisper.test/inference";
    config.stt_prompt = "Herdr, NixOS, Andromeda";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    http_handle = vocab_http;
    char out[128] = {0}, err[256] = {0};
    atomic_int cancelled = 0;
    int rc = audio_transcribe(audio, path, &cancelled, NULL, NULL, out, sizeof out, err, sizeof err);
    CHECK(rc == 0);
    CHECK(strcmp(out, "Check Herdr and NixOS.") == 0);
    unsigned char *saved = malloc(wav_len);
    FILE *file = fopen(path, "rb");
    CHECK(file && fread(saved, 1, wav_len, file) == wav_len);
    if (file) fclose(file);
    CHECK(saved && memcmp(saved, wav, wav_len) == 0);
    free(saved);
    CHECK(request_count >= 1);
    if (request_count) {
        CHECK(memmem(requests[0].body, requests[0].body_len, "name=\"prompt\"", 12) != NULL);
        CHECK(memmem(requests[0].body, requests[0].body_len, "Herdr, NixOS, Andromeda", 23) != NULL);
        CHECK(memmem(requests[0].body, requests[0].body_len, "name=\"file\"", 11) != NULL);
    }
    audio_free(audio);
    free(wav);
    rm_rf(dir);
    free(dir);
}

typedef struct speak_ctx {
    audio *audio;
    const char *text;
    atomic_int *cancelled;
    int rc;
    char err[512];
    gate done;
} speak_ctx;

static void *transcribe_main(void *arg) {
    speak_ctx *ctx = arg;
    char out[128];
    ctx->rc = audio_transcribe(ctx->audio, ctx->text, ctx->cancelled, NULL, NULL, out, sizeof out, ctx->err, sizeof ctx->err);
    if (ctx->rc == 0) snprintf(ctx->err, sizeof ctx->err, "%s", out);
    gate_set(&ctx->done);
    return NULL;
}

static int blocking_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    gate_set(&slow_entered);
    gate_wait(&slow_release, 3000);
    respond_text(response, 200, "{\"text\":\"late words\"}");
    return 0;
}

static void test_cancelled_recognition_keeps_audio(void) {
    test_name = "cancelled recognition keeps audio";
    use_key(NULL);
    char *dir = make_tmp();
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    write_file(path, wav, wav_len);
    audio_config config;
    audio_config_init(&config);
    config.stt_url = "http://whisper.test/inference";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    gate_init(&slow_entered);
    gate_init(&slow_release);
    http_handle = blocking_http;
    atomic_int cancelled = 0;
    speak_ctx ctx = {.audio = audio, .text = path, .cancelled = &cancelled};
    gate_init(&ctx.done);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, transcribe_main, &ctx) == 0);
    CHECK(gate_wait(&slow_entered, 2000));
    atomic_store(&cancelled, 1);
    CHECK(gate_wait(&ctx.done, 500));
    CHECK(ctx.rc == 0);
    CHECK(ctx.err[0] == 0);
    CHECK(access(path, F_OK) == 0);
    gate_set(&slow_release);
    join_ms(thread, 2000);
    audio_free(audio);
    gate_destroy(&slow_entered);
    gate_destroy(&slow_release);
    gate_destroy(&ctx.done);
    free(wav);
    rm_rf(dir);
    free(dir);
}

static gate second_entered;
static atomic_int speak_errors;

static int serial_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)user;
    int call = atomic_load(&http_calls);
    if (call == 1) {
        gate_set(&slow_entered);
        gate_wait(&slow_release, 3000);
    } else {
        gate_set(&second_entered);
    }
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len);
    free(wav);
    (void)request;
    return 0;
}

static void *serial_speak(void *arg) {
    speak_ctx *ctx = arg;
    ctx->rc = audio_speak(ctx->audio, ctx->text, ctx->cancelled, ctx->err, sizeof ctx->err);
    if (ctx->rc != 0) atomic_fetch_add(&speak_errors, 1);
    gate_set(&ctx->done);
    return NULL;
}

static void test_cancel_before_http_keeps_gpu_serial(void) {
    test_name = "cancel returns before http keeps gpu serial";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    reset_stats();
    clear_requests();
    gate_init(&slow_entered);
    gate_init(&slow_release);
    gate_init(&second_entered);
    http_handle = serial_http;
    atomic_store(&speak_errors, 0);
    atomic_int cancelled = 0;
    atomic_int none = 0;
    speak_ctx first = {.audio = audio, .text = "First.", .cancelled = &cancelled};
    speak_ctx second = {.audio = audio, .text = "Second.", .cancelled = &none};
    gate_init(&first.done);
    gate_init(&second.done);
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, serial_speak, &first) == 0);
    CHECK(gate_wait(&slow_entered, 2000));
    atomic_store(&cancelled, 1);
    audio_stop(audio);
    CHECK(gate_wait(&first.done, 500));
    CHECK(pthread_create(&b, NULL, serial_speak, &second) == 0);
    CHECK(!gate_wait(&second_entered, 200));
    gate_set(&slow_release);
    join_ms(a, 2000);
    join_ms(b, 2000);
    CHECK(atomic_load(&speak_errors) == 0);
    CHECK(gate_wait(&second_entered, 1000));
    int saw_first = 0, saw_second = 0, order = 1, seen = 0;
    char inputs[4][64];
    for (int i = 0; i < request_count && seen < 4; i++) {
        if (!json_field((char *)requests[i].body, requests[i].body_len, "input", inputs[seen], sizeof inputs[seen]))
            continue;
        if (strcmp(inputs[seen], "First.") == 0) saw_first = 1;
        if (strcmp(inputs[seen], "Second.") == 0) saw_second = 1;
        if (seen == 0 && strcmp(inputs[seen], "First.") != 0) order = 0;
        if (seen == 1 && strcmp(inputs[seen], "Second.") != 0) order = 0;
        seen++;
    }
    CHECK(saw_first && saw_second && order && seen == 2);
    audio_free(audio);
    gate_destroy(&slow_entered);
    gate_destroy(&slow_release);
    gate_destroy(&second_entered);
    gate_destroy(&first.done);
    gate_destroy(&second.done);
    rm_rf(dir);
    free(dir);
}

static int deepgram_listen_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 200,
        "{\"results\":{\"channels\":[{\"alternatives\":[{\"transcript\":\"hello NixOS\"}]}]}}");
    return 0;
}

static void test_deepgram_opt_out_and_keyterms(void) {
    test_name = "deepgram opt out and keyterms";
    const char *key = "dg-test-key";
    use_key(key);
    char *dir = make_tmp();
    char prefs[512], errlog[512];
    snprintf(prefs, sizeof prefs, "%s/voice-mode", dir);
    snprintf(errlog, sizeof errlog, "%s/stderr", dir);
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    write_file(path, wav, wav_len);
    audio_config config;
    audio_config_init(&config);
    config.stt_prompt = "NixOS, Pi";
    config.voice_preferences_path = prefs;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    http_handle = deepgram_listen_http;
    char err[256] = {0};
    CHECK(audio_set_stt_backend(audio, "deepgram", err, sizeof err) == 0);
    int saved_err = capture_stderr(errlog);
    char out[128] = {0};
    int rc = audio_transcribe(audio, path, NULL, NULL, NULL, out, sizeof out, err, sizeof err);
    restore_stderr(saved_err);
    CHECK(rc == 0);
    CHECK(strcmp(out, "hello NixOS") == 0);
    CHECK(request_count == 1);
    if (request_count) {
        CHECK(strstr(requests[0].url, "mip_opt_out=true") != NULL);
        CHECK(strstr(requests[0].url, "keyterm=NixOS") != NULL);
        CHECK(strstr(requests[0].url, "keyterm=Pi") != NULL);
        CHECK(requests[0].auth_matches);
        CHECK(!requests[0].key_in_url);
    }
    char stt_path[512];
    snprintf(stt_path, sizeof stt_path, "%s/stt-backend", dir);
    char *saved = read_text(stt_path);
    CHECK(saved && strcmp(saved, "deepgram\n") == 0);
    free(saved);
    expect_no_key_leak(err, dir, errlog);
    audio_free(audio);
    free(wav);
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static int voice_named(const audio_report *report, const char *id) {
    for (size_t i = 0; i < report->voice_count; i++) {
        if (strcmp(report->voice_ids[i], id) == 0) return 1;
    }
    return 0;
}

static int backend_named(const audio_report *report, const char *id) {
    for (size_t i = 0; i < report->speech_backend_count; i++) {
        if (strcmp(report->speech_backend_ids[i], id) == 0) return 1;
    }
    return 0;
}

static void test_catalogue_and_preferences(void) {
    test_name = "catalogue and preferences";
    use_key("dg-test-key");
    char *dir = make_tmp();
    char prefs[512];
    snprintf(prefs, sizeof prefs, "%s/voice-mode", dir);
    audio_config config;
    audio_config_init(&config);
    config.voice_preferences_path = prefs;
    audio *audio = audio_new(dir, &config);
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.speech_backend, "deepgram") == 0);
    CHECK(report.voice_count == 41);
    CHECK(!voice_named(&report, "samantha"));
    CHECK(voice_named(&report, "thalia"));
    char err[1024] = {0};
    CHECK(audio_set_voice(audio, "apollo", err, sizeof err) == 0);
    CHECK(audio_set_voice(audio, "samantha", err, sizeof err) == -1);
    CHECK(audio_set_speech_backend(audio, "local", err, sizeof err) == 0);
    CHECK(audio_status(audio, &report) == 0);
    CHECK(!voice_named(&report, "apollo"));
    CHECK(audio_set_voice(audio, "samantha", err, sizeof err) == 0);
    audio_free(audio);
    audio = audio_new(dir, &config);
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.speech_backend, "local") == 0);
    CHECK(audio_set_speech_backend(audio, "deepgram", err, sizeof err) == 0);
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.selected_voice, "apollo") == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static void test_cloud_only_and_missing_key(void) {
    test_name = "cloud only and missing key";
    use_key("dg-test-key");
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_enabled = 0;
    audio *cloud = audio_new(dir, &config);
    audio_report report;
    CHECK(audio_status(cloud, &report) == 0);
    CHECK(report.speech_backend_count == 1);
    CHECK(strcmp(report.speech_backend_ids[0], "deepgram") == 0);
    CHECK(strcmp(report.speech_backend_labels[0], "Deepgram") == 0);
    CHECK(!backend_named(&report, "local"));
    audio_free(cloud);
    use_key("");
    audio *silent = audio_new(dir, &config);
    CHECK(audio_status(silent, &report) == 0);
    CHECK(report.speech_backend_count == 0);
    CHECK(report.voice_count == 0);
    CHECK(report.stt_count == 2);
    CHECK(strcmp(report.stt_ids[0], "whisper") == 0);
    CHECK(strcmp(report.stt_ids[1], "deepgram") == 0);
    CHECK(strcmp(report.stt_labels[1], "Deepgram (cloud)") == 0);
    char err[256] = {0};
    CHECK(audio_set_stt_backend(silent, "deepgram", err, sizeof err) == -1);
    CHECK(strstr(err, "Deepgram API key is not installed") != NULL);
    CHECK(audio_set_speech_backend(silent, "deepgram", err, sizeof err) == -1);
    audio_free(silent);
    audio_config local_config;
    audio_config_init(&local_config);
    audio *local = audio_new(dir, &local_config);
    CHECK(audio_status(local, &report) == 0);
    CHECK(strcmp(report.speech_backend, "local") == 0);
    CHECK(backend_named(&report, "local"));
    CHECK(!backend_named(&report, "deepgram"));
    CHECK(report.stt_count == 2);
    audio_free(local);
    rm_rf(dir);
    free(dir);
}

static int saved_deepgram_without_key_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    fail_msg("dictation without a key must not send");
    response->transport_error = 1;
    return 0;
}

static void test_saved_dictation_fails_without_key(void) {
    test_name = "saved dictation fails without key";
    use_key(NULL);
    char *dir = make_tmp();
    char prefs[512];
    snprintf(prefs, sizeof prefs, "%s/stt-backend", dir);
    write_file(prefs, "deepgram\n", 9);
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    write_file(path, wav, wav_len);
    audio_config config;
    audio_config_init(&config);
    config.stt_preferences_path = prefs;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    http_handle = saved_deepgram_without_key_http;
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.selected_stt, "deepgram") == 0);
    CHECK(strcmp(report.stt_ids[1], "deepgram") == 0);
    char out[64] = {0}, err[256] = {0};
    int rc = audio_transcribe(audio, path, NULL, NULL, NULL, out, sizeof out, err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "Deepgram API key is not installed") != NULL);
    CHECK(atomic_load(&http_calls) == 0);
    audio_free(audio);
    free(wav);
    rm_rf(dir);
    free(dir);
}

static int wav_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_deepgram_request_skips_gpu(void) {
    test_name = "deepgram request skips gpu";
    use_key("dg-test-key");
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_health_url = "http://local-health.test/tts";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    reset_stats();
    clear_requests();
    atomic_store(&acquire_count, 0);
    http_handle = wav_http;
    char err[256] = {0};
    atomic_int cancelled = 0;
    int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
    CHECK(rc == 0);
    CHECK(request_count == 1);
    if (request_count) {
        CHECK(strstr(requests[0].url, "model=aura-2-thalia-en") != NULL);
        CHECK(strstr(requests[0].url, "container=wav") != NULL);
        CHECK(strstr(requests[0].url, "mip_opt_out=true") != NULL);
        CHECK(!requests[0].key_in_url);
        CHECK(requests[0].auth_matches);
        char text[64] = {0};
        CHECK(json_field((char *)requests[0].body, requests[0].body_len, "text", text, sizeof text));
        CHECK(strcmp(text, "Hello.") == 0);
    }
    CHECK(atomic_load(&acquire_count) == 0);
    CHECK(stats.opened == 1);
    CHECK(strcmp(stats.argv[0], "pw-play") == 0);
    CHECK(stats.rate == 24000);
    CHECK(stats.channels == 1);
    CHECK(stats.width == 2);
    CHECK(stats.fd_len >= 44);
    CHECK(stats.fd_len - 44 == 48);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static void test_playback_modes_bypass_local_readiness(void) {
    test_name = "playback modes bypass local readiness";
    use_key("dg-test-key");
    char *dir = make_tmp();
    const char *modes[] = {"buffered", "streaming"};
    for (int i = 0; i < 2; i++) {
        audio_config config;
        audio_config_init(&config);
        config.tts_health_url = "http://local-health.test/tts";
        config.playback_mode = modes[i];
        audio *audio = audio_new(dir, &config);
        attach_audio(audio);
        reset_stats();
        clear_requests();
        atomic_store(&acquire_count, 0);
        http_handle = wav_http;
        char err[256] = {0};
        atomic_int cancelled = 0;
        int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
        CHECK(rc == 0);
        CHECK(atomic_load(&acquire_count) == 0);
        CHECK(request_count == 1);
        if (request_count) CHECK(strstr(requests[0].url, "local-health.test") == NULL);
        CHECK(stats.opened == 1);
        CHECK(strcmp(stats.argv[0], "pw-play") == 0);
        if (strcmp(modes[i], "streaming") == 0) {
            CHECK(stats.pipe_stdin == 1);
            CHECK(argv_has("--raw"));
        }
        audio_free(audio);
    }
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static int health_error_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 500, "Local server unavailable");
    return 0;
}

static void test_cloud_status_hides_local_tts_error(void) {
    test_name = "cloud status hides local tts error";
    use_key("dg-test-key");
    char *dir = make_tmp();
    char prefs[512];
    snprintf(prefs, sizeof prefs, "%s/voice-mode", dir);
    audio_config config;
    audio_config_init(&config);
    config.tts_health_url = "http://tts.test/health";
    config.voice_preferences_path = prefs;
    config.readiness_timeout = 0.2;
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    http_handle = health_error_http;
    char err[512] = {0};
    CHECK(audio_set_speech_backend(audio, "local", err, sizeof err) == 0);
    CHECK(audio_wait_ready(audio, "tts", NULL, err, sizeof err) == -1);
    CHECK(audio_set_speech_backend(audio, "deepgram", err, sizeof err) == 0);
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(report.has_tts == 0);
    CHECK(report.has_tts_error == 0);
    CHECK(strcmp(report.speech_backend, "deepgram") == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static int private_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    respond_text(response, 401, "private");
    if (response->body && current_key[0]) {
        free(response->body);
        size_t n = strlen(current_key) + 16;
        response->body = malloc(n);
        snprintf((char *)response->body, n, "private %s", current_key);
        response->body_len = strlen((char *)response->body);
    }
    return 0;
}

static void test_error_hides_response_and_credentials(void) {
    test_name = "error hides response and credentials";
    const char *key = "dg-secret-token-9f3a";
    use_key(key);
    char *dir = make_tmp();
    char errlog[512];
    snprintf(errlog, sizeof errlog, "%s/stderr", dir);
    audio_config config;
    audio_config_init(&config);
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    http_handle = private_http;
    int saved = capture_stderr(errlog);
    char err[512] = {0};
    atomic_int cancelled = 0;
    int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
    restore_stderr(saved);
    CHECK(rc == -1);
    CHECK(strcmp(err, "Deepgram speech failed (HTTP 401)") == 0);
    expect_no_key_leak(err, dir, errlog);
    CHECK(strstr(err, "private") == NULL);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static void test_cancelled_deepgram_does_not_send(void) {
    test_name = "cancelled deepgram does not send";
    use_key("dg-test-key");
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    http_handle = wav_http;
    atomic_store(&acquire_count, 0);
    atomic_int cancelled = 1;
    char err[128] = {0};
    int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
    CHECK(rc == 0);
    CHECK(atomic_load(&http_calls) == 0);
    CHECK(atomic_load(&acquire_count) == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static int english_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_speech_request_selects_english(void) {
    test_name = "speech request selects english";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    reset_stats();
    http_handle = english_http;
    char err[256] = {0};
    atomic_int cancelled = 0;
    CHECK(audio_speak(audio, "Hello William.", &cancelled, err, sizeof err) == 0);
    CHECK(request_count == 1);
    if (request_count) {
        char language[32] = {0}, input[64] = {0};
        CHECK(json_field((char *)requests[0].body, requests[0].body_len, "language", language, sizeof language));
        CHECK(json_field((char *)requests[0].body, requests[0].body_len, "input", input, sizeof input));
        CHECK(strcmp(language, "English") == 0);
        CHECK(strcmp(input, "Hello William.") == 0);
    }
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static audio *character_audio;
static const char *switch_character;

static int switch_voice_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    char err[128];
    const char *other = switch_character && strcmp(switch_character, "samantha") == 0 ? "data" : "samantha";
    audio_set_voice(character_audio, other, err, sizeof err);
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_samantha_switches_reference_at_50_words(void) {
    test_name = "samantha switches reference at 50 words";
    use_key(NULL);
    char *dir = make_tmp();
    audio_pair data_opts[] = {
        {"voice_ref", "/data.wav"},
        {"reference_text", "Data reference."},
    };
    audio_voice voices[] = {{"data", "Data", data_opts, 2}};
    audio_pair long_voice[] = {
        {"voice_ref", "/newer.wav"},
        {"reference_text", "New reference."},
    };
    const char *modes[] = {"buffered", "streaming"};
    const char *characters[] = {"samantha", "samantha", "data", "data"};
    int counts[] = {50, 51, 1, 51};
    const char *expected[] = {NULL, "/newer.wav", "/data.wav", "/data.wav"};
    const char *expected_text[] = {NULL, "New reference.", "Data reference.", "Data reference."};
    for (int mode = 0; mode < 2; mode++) {
        for (int i = 0; i < 4; i++) {
            audio_config config;
            audio_config_init(&config);
            config.tts_url = "http://tts.test/speech";
            config.playback_mode = modes[mode];
            config.voices = voices;
            config.voice_count = 1;
            config.long_voice = long_voice;
            config.long_voice_count = 2;
            audio *audio = audio_new(dir, &config);
            attach_audio(audio);
            character_audio = audio;
            switch_character = characters[i];
            char err[256] = {0};
            CHECK(audio_set_voice(audio, characters[i], err, sizeof err) == 0);
            clear_requests();
            reset_stats();
            http_handle = switch_voice_http;
            char *text = repeat("word ", counts[i]);
            atomic_int cancelled = 0;
            int rc = audio_speak(audio, text, &cancelled, err, sizeof err);
            CHECK(rc == 0);
            CHECK(request_count > 0);
            int total = 0;
            for (int r = 0; r < request_count; r++) {
                char input[512] = {0}, ref[64] = {0}, reference[64] = {0};
                CHECK(json_field((char *)requests[r].body, requests[r].body_len, "input", input, sizeof input));
                total += words_in(input);
                int has_ref = json_field((char *)requests[r].body, requests[r].body_len, "voice_ref", ref, sizeof ref);
                int has_text = json_field((char *)requests[r].body, requests[r].body_len, "reference_text", reference, sizeof reference);
                if (!expected[i]) {
                    CHECK(!has_ref);
                    CHECK(!has_text);
                    CHECK(field_absent((char *)requests[r].body, requests[r].body_len, "voice_ref"));
                } else {
                    CHECK(has_ref && strcmp(ref, expected[i]) == 0);
                    CHECK(has_text && strcmp(reference, expected_text[i]) == 0);
                }
            }
            CHECK(total == counts[i]);
            free(text);
            audio_free(audio);
        }
    }
    rm_rf(dir);
    free(dir);
}

static void test_character_choice_persists(void) {
    test_name = "character choice persists";
    use_key(NULL);
    char *dir = make_tmp();
    char prefs[512];
    snprintf(prefs, sizeof prefs, "%s/voice-mode", dir);
    audio_pair data_opts[] = {
        {"voice_ref", "/data.wav"},
        {"reference_text", "Words."},
    };
    audio_voice voices[] = {{"data", "Data", data_opts, 2}};
    audio_config config;
    audio_config_init(&config);
    config.voice_preferences_path = prefs;
    config.voices = voices;
    config.voice_count = 1;
    const char *old[] = {"auto", "current", "newer", "removed-character"};
    for (int i = 0; i < 4; i++) {
        char line[64];
        snprintf(line, sizeof line, "%s\n", old[i]);
        write_file(prefs, line, strlen(line));
        audio *audio = audio_new(dir, &config);
        audio_report report;
        CHECK(audio_status(audio, &report) == 0);
        CHECK(strcmp(report.selected_voice, "samantha") == 0);
        audio_free(audio);
    }
    audio *audio = audio_new(dir, &config);
    char err[1024] = {0};
    CHECK(audio_set_voice(audio, "data", err, sizeof err) == 0);
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.selected_voice, "data") == 0);
    audio_free(audio);
    audio = audio_new(dir, &config);
    CHECK(audio_status(audio, &report) == 0);
    CHECK(strcmp(report.selected_voice, "data") == 0);
    CHECK(audio_set_voice(audio, "unknown", err, sizeof err) == -1);
    CHECK(strstr(err, "Unknown voice") != NULL);
    char *saved = read_text(prefs);
    CHECK(saved && strcmp(saved, "data\n") == 0);
    free(saved);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static unsigned char *pattern_wav(unsigned char value, int samples, size_t *len) {
    size_t nbytes = (size_t)samples * 2;
    unsigned char *frames = malloc(nbytes);
    for (int i = 0; i < samples; i++) {
        frames[i * 2] = value;
        frames[i * 2 + 1] = 0;
    }
    unsigned char *wav = build_wav(1, 24000, 2, frames, nbytes, len);
    free(frames);
    return wav;
}

static int two_sample_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request;
    int call = (int)(intptr_t)user;
    size_t len = 0;
    unsigned char *wav = pattern_wav(call == 1 ? 0x11 : 0x22, call == 1 ? 240 : 480, &len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_buffered_waits_for_every_chunk(void) {
    test_name = "buffered waits for every chunk";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    reset_stats();
    http_handle = two_sample_http;
    char *text = repeat("First ", 40);
    char *second = repeat("second ", 10);
    char spoken[512];
    snprintf(spoken, sizeof spoken, "%s%s", text, second);
    free(text);
    free(second);
    char err[256] = {0};
    atomic_int cancelled = 0;
    CHECK(audio_speak(audio, spoken, &cancelled, err, sizeof err) == 0);
    CHECK(stats.opened == 1);
    CHECK(stats.http_at_open == 2);
    CHECK(strcmp(stats.argv[0], "pw-play") == 0);
    CHECK(strstr(stats.argv[1], "/proc/self/fd/") == stats.argv[1]);
    CHECK(stats.rate == 24000);
    CHECK(stats.channels == 1);
    CHECK(stats.width == 2);
    CHECK(stats.wait_calls == 1);
    CHECK(stats.last_wait_ms < 0);
    CHECK(stats.fd_len == 44 + (240 + 480) * 2);
    if (stats.fd_len >= 44) {
        CHECK(stats.fd_bytes[44] == 0x11);
        CHECK(stats.fd_bytes[44 + 480] == 0x22);
    }
    CHECK(count_suffix(dir, ".wav") == 0);
    CHECK(!audio_playing(audio));
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static gate writing;
static gate prepared;

static int prefetch_http(const audio_http_request *request, audio_http_response *response, void *user) {
    int call = (int)(intptr_t)user;
    (void)request;
    if (call == 2) {
        if (!gate_wait(&writing, 2000)) {
            response->transport_error = 1;
            return 0;
        }
        gate_set(&prepared);
    }
    size_t len = 0;
    unsigned char *wav = pattern_wav(call == 1 ? 0x11 : 0x22, call == 1 ? 240 : 480, &len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void first_write_waits(fake_player *player, const void *data, size_t len) {
    (void)player; (void)data; (void)len;
    pthread_mutex_lock(&stats_mu);
    int index = stats.nwrites;
    pthread_mutex_unlock(&stats_mu);
    if (index != 1) return;
    gate_set(&writing);
    if (!gate_wait(&prepared, 2000)) fail_msg("next chunk was not prepared during playback");
}

static void test_streaming_prepares_next_chunk(void) {
    test_name = "streaming prepares next chunk";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    config.playback_mode = "streaming";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    clear_requests();
    reset_stats();
    gate_init(&writing);
    gate_init(&prepared);
    http_handle = prefetch_http;
    write_hook = first_write_waits;
    char *text = repeat("First ", 40);
    char *second = repeat("second ", 10);
    char spoken[512];
    snprintf(spoken, sizeof spoken, "%s%s", text, second);
    free(text);
    free(second);
    char err[256] = {0};
    atomic_int cancelled = 0;
    CHECK(audio_speak(audio, spoken, &cancelled, err, sizeof err) == 0);
    CHECK(stats.nwrites == 2);
    if (stats.nwrites == 2) {
        CHECK(stats.write_len[0] == 480 && stats.writes[0][0] == 0x11);
        CHECK(stats.write_len[1] == 960 && stats.writes[1][0] == 0x22);
    }
    CHECK(stats.opened == 1);
    CHECK(argv_has("--raw"));
    CHECK(stats.pipe_stdin == 1);
    CHECK(strcmp(stats.argv[0], "pw-play") == 0);
    CHECK(stats.close_calls == 1);
    CHECK(stats.wait_calls >= 1);
    CHECK(stats.last_wait_ms < 0);
    CHECK(!audio_playing(audio));
    audio_free(audio);
    gate_destroy(&writing);
    gate_destroy(&prepared);
    rm_rf(dir);
    free(dir);
}

static gate preparing;
static gate stopped;
static audio *stop_audio;
static atomic_int *stop_flag;

static int cancel_prefetch_http(const audio_http_request *request, audio_http_response *response, void *user) {
    int call = (int)(intptr_t)user;
    (void)request;
    if (call == 2) {
        gate_set(&preparing);
        gate_wait(&stopped, 2000);
    }
    size_t len = 0;
    unsigned char *wav = pattern_wav(0x11, 240, &len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void cancel_on_write(fake_player *player, const void *data, size_t len) {
    (void)player; (void)data; (void)len;
    if (!gate_wait(&preparing, 2000)) fail_msg("prefetch did not start");
    if (stop_flag) atomic_store(stop_flag, 1);
    audio_stop(stop_audio);
    gate_set(&stopped);
}

static void test_streaming_cancel_discards_prefetch(void) {
    test_name = "streaming cancel discards prefetch";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    config.playback_mode = "streaming";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    stop_audio = audio;
    reset_stats();
    clear_requests();
    gate_init(&preparing);
    gate_init(&stopped);
    http_handle = cancel_prefetch_http;
    write_hook = cancel_on_write;
    atomic_int cancelled = 0;
    stop_flag = &cancelled;
    char *text = repeat("First ", 40);
    char *second = repeat("second ", 10);
    char spoken[512];
    snprintf(spoken, sizeof spoken, "%s%s", text, second);
    free(text);
    free(second);
    char err[256] = {0};
    CHECK(audio_speak(audio, spoken, &cancelled, err, sizeof err) == 0);
    CHECK(stats.nwrites == 1);
    if (stats.nwrites == 1) CHECK(stats.write_len[0] == 480);
    CHECK(stats.terminate == 1);
    CHECK(stats.close_calls == 1);
    CHECK(!audio_playing(audio));
    audio_free(audio);
    gate_destroy(&preparing);
    gate_destroy(&stopped);
    rm_rf(dir);
    free(dir);
}

static int stream_fail_mode;

static int stream_fail_http(const audio_http_request *request, audio_http_response *response, void *user) {
    int call = (int)(intptr_t)user;
    (void)request;
    if (call == 1) {
        size_t len = 0;
        unsigned char *wav = pattern_wav(0x11, 240, &len);
        respond_bin(response, 200, wav, len);
        free(wav);
        return 0;
    }
    if (stream_fail_mode == 0) {
        snprintf(response->verbatim_error, sizeof response->verbatim_error, "Synthesis timed out");
        return 0;
    }
    size_t len = 0;
    unsigned char *wav = pattern_wav(0x11, 240, &len);
    respond_bin(response, 200, wav, len > 2 ? len - 2 : len);
    free(wav);
    return 0;
}

static void test_streaming_failure_stops_playback(void) {
    test_name = "streaming failure stops playback";
    use_key(NULL);
    char *dir = make_tmp();
    char *text = repeat("First ", 40);
    char *second = repeat("second ", 10);
    char spoken[512];
    snprintf(spoken, sizeof spoken, "%s%s", text, second);
    free(text);
    free(second);
    for (stream_fail_mode = 0; stream_fail_mode < 2; stream_fail_mode++) {
        audio_config config;
        audio_config_init(&config);
        config.tts_url = "http://tts.test/speech";
        config.playback_mode = "streaming";
        audio *audio = audio_new(dir, &config);
        attach_audio(audio);
        reset_stats();
        clear_requests();
        http_handle = stream_fail_http;
        write_hook = NULL;
        char err[512] = {0};
        atomic_int cancelled = 0;
        int rc = audio_speak(audio, spoken, &cancelled, err, sizeof err);
        CHECK(rc == -1);
        if (stream_fail_mode == 0) CHECK(strstr(err, "Synthesis timed out") != NULL);
        else CHECK(strstr(err, "Incomplete speech audio") != NULL);
        CHECK(stats.opened == 1);
        CHECK(stats.terminate == 1);
        CHECK(stats.wait_calls == 1);
        CHECK(stats.last_wait_ms == 5000);
        CHECK(stats.close_calls == 1);
        CHECK(!audio_playing(audio));
        /* A failed later chunk must not be reported as a completed reply. */
        CHECK(stats.nwrites == 1);
        audio_free(audio);
    }
    rm_rf(dir);
    free(dir);
}

static atomic_int *first_cancel;

static int cancel_during_first_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    if (first_cancel) atomic_store(first_cancel, 1);
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_cancelling_before_first_streamed_chunk(void) {
    test_name = "cancelling before first streamed chunk";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    config.playback_mode = "streaming";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    reset_stats();
    clear_requests();
    atomic_int cancelled = 0;
    first_cancel = &cancelled;
    http_handle = cancel_during_first_http;
    char err[256] = {0};
    CHECK(audio_speak(audio, "Hello William.", &cancelled, err, sizeof err) == 0);
    CHECK(stats.opened == 0);
    CHECK(atomic_load(&http_calls) == 1);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static int cancel_on_second_http(const audio_http_request *request, audio_http_response *response, void *user) {
    int call = (int)(intptr_t)user;
    (void)request;
    if (call == 2 && first_cancel) atomic_store(first_cancel, 1);
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_cancelling_buffered_discards_before_playback(void) {
    test_name = "cancelling buffered discards before playback";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    reset_stats();
    clear_requests();
    atomic_int cancelled = 0;
    first_cancel = &cancelled;
    http_handle = cancel_on_second_http;
    char *text = repeat("A longer reply. ", 60);
    char err[256] = {0};
    CHECK(audio_speak(audio, text, &cancelled, err, sizeof err) == 0);
    free(text);
    CHECK(atomic_load(&http_calls) == 2);
    CHECK(stats.opened == 0);
    CHECK(count_suffix(dir, ".wav") == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static int timeout_on_second_http(const audio_http_request *request, audio_http_response *response, void *user) {
    int call = (int)(intptr_t)user;
    (void)request;
    if (call == 2) {
        snprintf(response->verbatim_error, sizeof response->verbatim_error, "Synthesis timed out");
        return 0;
    }
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_later_failure_never_plays_partial(void) {
    test_name = "later failure never plays partial";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    reset_stats();
    clear_requests();
    http_handle = timeout_on_second_http;
    char *text = repeat("A longer reply. ", 60);
    char err[512] = {0};
    atomic_int cancelled = 0;
    int rc = audio_speak(audio, text, &cancelled, err, sizeof err);
    free(text);
    CHECK(rc == -1);
    CHECK(strstr(err, "Synthesis timed out") != NULL);
    CHECK(stats.opened == 0);
    CHECK(count_suffix(dir, ".wav") == 0);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static atomic_int child_calls;

static int exit_on_second_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    int call = atomic_fetch_add(&child_calls, 1) + 1;
    if (call == 2) _exit(0);
    size_t len = 0;
    unsigned char *wav = pattern_wav(0x11, 24000, &len);
    respond_bin(response, 200, wav, len);
    free(wav);
    return 0;
}

static void test_exit_during_synthesis_leaves_no_buffer(void) {
    test_name = "exit during synthesis leaves no buffer";
    use_key(NULL);
    char *dir = make_tmp();
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        audio_config config;
        audio_config_init(&config);
        config.tts_url = "http://tts.test/speech";
        audio *audio = audio_new(dir, &config);
        audio_set_http(audio, exit_on_second_http, NULL);
        audio_set_popen(audio, record_popen, NULL);
        atomic_store(&child_calls, 0);
        char *text = repeat("A longer reply. ", 60);
        char err[256];
        atomic_int cancelled = 0;
        audio_speak(audio, text, &cancelled, err, sizeof err);
        _exit(2);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(count_suffix(dir, ".wav") == 0);
    CHECK(dir_entries(dir) == 0);
    rm_rf(dir);
    free(dir);
}

static int invalid_later_mode;

static int invalid_later_http(const audio_http_request *request, audio_http_response *response, void *user) {
    int call = (int)(intptr_t)user;
    (void)request;
    if (call == 1 || invalid_later_mode == 0) {
        size_t len = 0;
        unsigned char *wav = silence_wav(&len);
        if (call == 2 && invalid_later_mode == 1 && len > 2) {
            respond_bin(response, 200, wav, len - 2);
        } else if (call == 2 && invalid_later_mode == 0) {
            unsigned char frames[96] = {0};
            size_t stereo_len = 0;
            unsigned char *stereo = build_wav(2, 24000, 2, frames, sizeof frames, &stereo_len);
            respond_bin(response, 200, stereo, stereo_len);
            free(stereo);
        } else {
            respond_bin(response, 200, wav, len);
        }
        free(wav);
        return 0;
    }
    size_t len = 0;
    unsigned char *wav = silence_wav(&len);
    respond_bin(response, 200, wav, len > 2 ? len - 2 : len);
    free(wav);
    return 0;
}

static void test_invalid_later_wav_discards_buffer(void) {
    test_name = "invalid later wav discards buffer";
    use_key(NULL);
    char *dir = make_tmp();
    const char *errors[] = {"Speech audio format changed", "Incomplete speech audio"};
    for (invalid_later_mode = 0; invalid_later_mode < 2; invalid_later_mode++) {
        audio_config config;
        audio_config_init(&config);
        config.tts_url = "http://tts.test/speech";
        audio *audio = audio_new(dir, &config);
        attach_audio(audio);
        reset_stats();
        clear_requests();
        http_handle = invalid_later_http;
        char *text = repeat("A longer reply. ", 60);
        char err[512] = {0};
        atomic_int cancelled = 0;
        int rc = audio_speak(audio, text, &cancelled, err, sizeof err);
        free(text);
        CHECK(rc == -1);
        CHECK(strstr(err, errors[invalid_later_mode]) != NULL);
        CHECK(stats.opened == 0);
        CHECK(count_suffix(dir, ".wav") == 0);
        audio_free(audio);
    }
    rm_rf(dir);
    free(dir);
}

static void test_menu_fields_available_on_status(void) {
    test_name = "active catalogue is visible on status";
    use_key("dg-test-key");
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    audio *audio = audio_new(dir, &config);
    audio_report report;
    CHECK(audio_status(audio, &report) == 0);
    CHECK(voice_named(&report, "thalia"));
    CHECK(!voice_named(&report, "samantha"));
    CHECK(backend_named(&report, "deepgram"));
    CHECK(strcmp(report.speech_backend, "deepgram") == 0);
    CHECK(strcmp(report.speech_backend_labels[0], "Local characters") == 0 ||
        strcmp(report.speech_backend_labels[1], "Deepgram") == 0);
    int saw_deepgram = 0;
    for (size_t i = 0; i < report.speech_backend_count; i++) {
        if (strcmp(report.speech_backend_ids[i], "deepgram") == 0 &&
            strcmp(report.speech_backend_labels[i], "Deepgram") == 0)
            saw_deepgram = 1;
    }
    CHECK(saw_deepgram);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
    use_key(NULL);
}

static int utf8_body_mode;
static int utf8_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    if (utf8_body_mode == 0) respond_text(response, 200, "{\"text\":\"caf\xc3\xa9\"}");
    else if (utf8_body_mode == 1) respond_text(response, 200, "{\"text\":\"\\uD83D\\uDE00\"}");
    else respond_text(response, 200, "{\"text\":\"\\uD800\"}");
    return 0;
}

static void test_json_keeps_raw_utf8_and_surrogate_pairs(void) {
    test_name = "json keeps raw utf8 and surrogate pairs";
    use_key(NULL);
    char *dir = make_tmp();
    size_t wav_len = 0;
    unsigned char *wav = silence_wav(&wav_len);
    char path[512];
    snprintf(path, sizeof path, "%s/dictation.wav", dir);
    write_file(path, wav, wav_len);
    audio_config config;
    audio_config_init(&config);
    config.stt_url = "http://stt.test/inference";
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    http_handle = utf8_http;
    char out[64] = {0}, err[256] = {0};
    utf8_body_mode = 0;
    CHECK(audio_transcribe(audio, path, NULL, NULL, NULL, out, sizeof out, err, sizeof err) == 0);
    CHECK(strcmp(out, "caf\xc3\xa9") == 0);
    utf8_body_mode = 1;
    memset(out, 0, sizeof out);
    CHECK(audio_transcribe(audio, path, NULL, NULL, NULL, out, sizeof out, err, sizeof err) == 0);
    CHECK(strcmp(out, "\xf0\x9f\x98\x80") == 0);
    utf8_body_mode = 2;
    CHECK(audio_transcribe(audio, path, NULL, NULL, NULL, out, sizeof out, err, sizeof err) == -1);
    CHECK(strstr(err, "invalid JSON") != NULL);
    audio_free(audio);
    free(wav);
    rm_rf(dir);
    free(dir);
}

static int bad_wav_mode;
static int bad_wav_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    if (bad_wav_mode == 0) {
        size_t len = 0;
        unsigned char *wav = silence_wav(&len);
        if (wav && len > 20) wav[20] = 3;
        respond_bin(response, 200, wav, len);
        free(wav);
        return 0;
    }
    unsigned char junk[12 + 8 + 4 + 8 + 16 + 8 + 4];
    memset(junk, 0, sizeof junk);
    memcpy(junk, "RIFF", 4);
    memcpy(junk + 8, "WAVE", 4);
    memcpy(junk + 12, "JUNK", 4);
    junk[16] = 4;
    memcpy(junk + 24, "fmt ", 4);
    junk[28] = 16;
    junk[32] = 1;
    junk[34] = 1;
    junk[36] = 0xC0; junk[37] = 0x5D;
    junk[46] = 16;
    memcpy(junk + 48, "data", 4);
    junk[52] = 100;
    respond_bin(response, 200, junk, sizeof junk);
    return 0;
}

static void test_wav_rejects_non_pcm_and_short_data_after_junk(void) {
    test_name = "wav rejects non-pcm and short data after junk";
    use_key(NULL);
    char *dir = make_tmp();
    const char *modes[] = {"buffered", "streaming"};
    for (int mode = 0; mode < 2; mode++) {
        for (bad_wav_mode = 0; bad_wav_mode < 2; bad_wav_mode++) {
            audio_config config;
            audio_config_init(&config);
            config.tts_url = "http://tts.test/speech";
            config.playback_mode = modes[mode];
            audio *audio = audio_new(dir, &config);
            attach_audio(audio);
            reset_stats();
            http_handle = bad_wav_http;
            char err[256] = {0};
            atomic_int cancelled = 0;
            int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
            CHECK(rc == -1);
            CHECK(strstr(err, "Incomplete speech audio") != NULL);
            CHECK(stats.opened == 0);
            audio_free(audio);
        }
    }
    rm_rf(dir);
    free(dir);
}

static void test_speech_chunks_follow_utf8_boundaries(void) {
    test_name = "speech chunks follow utf8 boundaries";
    char text[130 * 4 + 1];
    for (int i = 0; i < 130; i++) memcpy(text + i * 4, "\xf0\x9f\x98\x80", 4);
    text[130 * 4] = 0;
    char **chunks = NULL;
    size_t count = 0;
    CHECK(audio_speech_chunks(text, &chunks, &count) == 0);
    CHECK(count == 2);
    if (count == 2) {
        CHECK(strlen(chunks[0]) == 480);
        CHECK(strlen(chunks[1]) == 40);
    }
    free_chunks(chunks, count);
    chunks = NULL;
    count = 0;
    CHECK(audio_speech_chunks("\xff", &chunks, &count) == -1);
    free_chunks(chunks, count);
}

static void test_preference_close_failure_is_single(void) {
    test_name = "preference close failure is single";
    if (access("/dev/full", W_OK) != 0) {
        fprintf(stderr, "SKIP preference close: no /dev/full\n");
        return;
    }
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.voice_preferences_path = "/dev/full";
    audio *audio = audio_new(dir, &config);
    char err[128] = {0};
    int rc = audio_set_voice(audio, "samantha", err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "Could not save") != NULL);
    audio_free(audio);
    rm_rf(dir);
    free(dir);
}

static gate token_entered;
static gate token_release;

static int token_op(void *user, char *err, size_t err_cap) {
    (void)user; (void)err; (void)err_cap;
    gate_set(&token_entered);
    if (!gate_wait(&token_release, 2000)) return -1;
    return 0;
}

static void *store_cancel(void *arg) {
    atomic_int *flag = arg;
    if (!gate_wait(&token_entered, 1000)) return NULL;
    atomic_store(flag, 1);
    return NULL;
}

static void test_abandoned_worker_drops_cancel_token(void) {
    test_name = "abandoned worker drops cancel token";
    use_key(NULL);
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    audio *audio = audio_new(dir, &config);
    attach_audio(audio);
    atomic_int *flag = malloc(sizeof *flag);
    CHECK(flag != NULL);
    if (flag) atomic_init(flag, 0);
    gate_init(&token_entered);
    gate_init(&token_release);
    pthread_t setter;
    CHECK(pthread_create(&setter, NULL, store_cancel, flag) == 0);
    char err[128] = {0};
    int rc = audio_call(audio, "stt", flag, token_op, NULL, err, sizeof err);
    CHECK(rc == 1);
    free(flag);
    gate_set(&token_release);
    CHECK(join_ms(setter, 1000) == 0);
    audio_free(audio);
    gate_destroy(&token_entered);
    gate_destroy(&token_release);
    rm_rf(dir);
    free(dir);
}

static gate spawn_entered;
static gate spawn_release;

static audio_player *hold_popen(const audio_spawn *spawn, void *user) {
    gate_set(&spawn_entered);
    if (!gate_wait(&spawn_release, 2000)) return NULL;
    return record_popen(spawn, user);
}

static void *speak_hello(void *arg) {
    hold_ctx *ctx = arg;
    char err[256] = {0};
    ctx->rc = audio_speak(ctx->audio, "Hello.", ctx->cancelled, err, sizeof err);
    return NULL;
}

static void test_stop_during_player_spawn_discards(void) {
    test_name = "stop during player spawn discards";
    use_key(NULL);
    char *dir = make_tmp();
    const char *modes[] = {"buffered", "streaming"};
    for (int i = 0; i < 2; i++) {
        block_until_stop = 1;
        spawn_wait_code = -1;
        audio_config config;
        audio_config_init(&config);
        config.tts_url = "http://tts.test/speech";
        config.playback_mode = modes[i];
        audio *audio = audio_new(dir, &config);
        attach_audio(audio);
        audio_set_popen(audio, hold_popen, NULL);
        reset_stats();
        http_handle = english_http;
        gate_init(&spawn_entered);
        gate_init(&spawn_release);
        hold_ctx ctx = {.audio = audio};
        atomic_int cancelled = 0;
        ctx.cancelled = &cancelled;
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, speak_hello, &ctx) == 0);
        CHECK(gate_wait(&spawn_entered, 1000));
        audio_stop(audio);
        gate_set(&spawn_release);
        CHECK(join_ms(thread, 1000) == 0);
        CHECK(ctx.rc == 0);
        CHECK(stats.terminate >= 1);
        CHECK(!audio_playing(audio));
        audio_free(audio);
        gate_destroy(&spawn_entered);
        gate_destroy(&spawn_release);
    }
    block_until_stop = 0;
    spawn_wait_code = 0;
    rm_rf(dir);
    free(dir);
}

static int write_script(const char *path, const char *body) {
    FILE *file = fopen(path, "w");
    if (!file) return -1;
    if (fputs(body, file) < 0) { fclose(file); return -1; }
    if (fclose(file) != 0) return -1;
    return chmod(path, 0755);
}

static char *saved_path;

static void push_path(const char *dir) {
    const char *old = getenv("PATH");
    free(saved_path);
    saved_path = strdup(old ? old : "");
    size_t n = strlen(dir) + strlen(saved_path) + 2;
    char *buf = malloc(n);
    if (!buf) return;
    snprintf(buf, n, "%s:%s", dir, saved_path);
    setenv("PATH", buf, 1);
    free(buf);
}

static void pop_path(void) {
    if (saved_path) setenv("PATH", saved_path, 1);
    free(saved_path);
    saved_path = NULL;
}

static int large_pcm_http(const audio_http_request *request, audio_http_response *response, void *user) {
    (void)request; (void)user;
    size_t nbytes = 200000;
    unsigned char *pcm = calloc(1, nbytes);
    size_t len = 0;
    unsigned char *wav = build_wav(1, 24000, 2, pcm, nbytes, &len);
    respond_bin(response, 200, wav, len);
    free(wav);
    free(pcm);
    return 0;
}

static void test_buffered_spawn_receives_private_wav(void) {
    test_name = "buffered spawn receives private wav";
    use_key(NULL);
    char *dir = make_tmp();
    char script[512], played[512], body[700];
    snprintf(script, sizeof script, "%s/pw-play", dir);
    snprintf(played, sizeof played, "%s/played.wav", dir);
    snprintf(body, sizeof body, "#!/bin/sh\ncp \"$1\" '%s'\n", played);
    CHECK(write_script(script, body) == 0);
    push_path(dir);
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    config.playback_mode = "buffered";
    audio *audio = audio_new(dir, &config);
    audio_set_http(audio, large_pcm_http, NULL);
    char err[256] = {0};
    atomic_int cancelled = 0;
    CHECK(audio_speak(audio, "Hello.", &cancelled, err, sizeof err) == 0);
    struct stat st;
    CHECK(stat(played, &st) == 0 && st.st_size == 200044);
    audio_free(audio);
    pop_path();
    rm_rf(dir);
    free(dir);
}

static void test_closed_player_pipe_does_not_raise_sigpipe(void) {
    test_name = "closed player pipe does not raise sigpipe";
    use_key(NULL);
    char *dir = make_tmp();
    char script[512], pidfile[512];
    snprintf(script, sizeof script, "%s/pw-play", dir);
    snprintf(pidfile, sizeof pidfile, "%s/player.pid", dir);
    char body[640];
    snprintf(body, sizeof body, "#!/bin/sh\necho $$ > '%s'\nexec 0<&-\nexec sleep 30\n", pidfile);
    CHECK(write_script(script, body) == 0);
    push_path(dir);
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    config.playback_mode = "streaming";
    audio *audio = audio_new(dir, &config);
    audio_set_http(audio, large_pcm_http, NULL);
    audio_set_popen(audio, NULL, NULL);
    char err[256] = {0};
    atomic_int cancelled = 0;
    int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "playback") != NULL);
    FILE *pidf = fopen(pidfile, "r");
    int pid = 0;
    if (!pidf || fscanf(pidf, "%d", &pid) != 1) fail_msg("player pid missing");
    if (pidf) fclose(pidf);
    if (pid > 0) {
        int status = 0;
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) fail_msg("player leaked a zombie");
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            fail_msg("player still running");
        }
    }
    audio_free(audio);
    pop_path();
    rm_rf(dir);
    free(dir);
}

static int fd_recorded(const int *fds, int count, int fd) {
    for (int i = 0; i < count; i++) if (fds[i] == fd) return 1;
    return 0;
}

static int snapshot_fds(int *fds, int cap) {
    int n = 0;
    for (int fd = 0; fd < cap; fd++) {
        if (fcntl(fd, F_GETFD) >= 0) fds[n++] = fd;
    }
    return n;
}

static void *speak_blocking(void *arg) {
    hold_ctx *ctx = arg;
    char err[256] = {0};
    ctx->rc = audio_speak(ctx->audio, "Hello.", ctx->cancelled, err, sizeof err);
    return NULL;
}

static void test_player_pipe_is_not_inherited_by_capture(void) {
    test_name = "player pipe is not inherited by capture";
    use_key(NULL);
    char *dir = make_tmp();
    char script[512], pidfile[512], fdfile[512], capwav[512];
    snprintf(script, sizeof script, "%s/pw-play", dir);
    snprintf(pidfile, sizeof pidfile, "%s/player.pid", dir);
    snprintf(fdfile, sizeof fdfile, "%s/child-fds", dir);
    snprintf(capwav, sizeof capwav, "%s/capture.wav", dir);
    char body[640];
    snprintf(body, sizeof body, "#!/bin/sh\necho $$ > '%s'\nexec sleep 30\n", pidfile);
    CHECK(write_script(script, body) == 0);
    push_path(dir);
    int before[4096];
    int nbefore = snapshot_fds(before, 4096);
    audio_config config;
    audio_config_init(&config);
    config.tts_url = "http://tts.test/speech";
    config.playback_mode = "streaming";
    audio *audio = audio_new(dir, &config);
    audio_set_http(audio, large_pcm_http, NULL);
    audio_set_popen(audio, NULL, NULL);
    hold_ctx ctx = {.audio = audio};
    atomic_int cancelled = 0;
    ctx.cancelled = &cancelled;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, speak_blocking, &ctx) == 0);
    int playing = 0;
    for (int i = 0; i < 200; i++) {
        if (audio_playing(audio)) { playing = 1; break; }
        sleep_ms(10);
    }
    CHECK(playing);
    struct stat pipes[8];
    int npipes = 0;
    for (int fd = 0; fd < 4096 && npipes < 8; fd++) {
        struct stat st;
        if (fd_recorded(before, nbefore, fd) || fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) continue;
        int flags = fcntl(fd, F_GETFD);
        CHECK(flags >= 0 && (flags & FD_CLOEXEC));
        pipes[npipes++] = st;
    }
    CHECK(npipes > 0);
    capture_test_reset();
    char py[1024];
    snprintf(py, sizeof py,
        "import os\n"
        "out=open('%s','w')\n"
        "for name in os.listdir('/proc/self/fd'):\n"
        "    try:\n"
        "        st=os.stat('/proc/self/fd/'+name)\n"
        "    except OSError:\n"
        "        continue\n"
        "    out.write('%%d %%d\\n'%%(st.st_dev, st.st_ino))\n",
        fdfile);
    char *argv[] = {"python3", "-c", py, NULL};
    PipeWireCapture *capture = capture_open(capwav, argv, NULL);
    int code = -1;
    if (!capture || capture_wait(capture, 2, &code) != 0) fail_msg("capture did not finish");
    capture_free(capture);
    FILE *fds = fopen(fdfile, "r");
    if (!fds) fail_msg("child fd list missing");
    if (fds) {
        char line[80];
        while (fgets(line, sizeof line, fds)) {
            unsigned long dev = 0, ino = 0;
            if (sscanf(line, "%lu %lu", &dev, &ino) != 2) continue;
            for (int i = 0; i < npipes; i++) {
                if (dev == (unsigned long)pipes[i].st_dev && ino == (unsigned long)pipes[i].st_ino)
                    fail_msg("capture inherited player pipe");
            }
        }
        fclose(fds);
    }
    audio_stop(audio);
    CHECK(join_ms(thread, 2000) == 0);
    FILE *pidf = fopen(pidfile, "r");
    int pid = 0;
    if (!pidf || fscanf(pidf, "%d", &pid) != 1) fail_msg("player pid missing");
    if (pidf) fclose(pidf);
    if (pid > 0) {
        int status = 0;
        pid_t got = waitpid((pid_t)pid, &status, WNOHANG);
        if (got == (pid_t)pid) fail_msg("stopped player leaked a zombie");
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
            waitpid((pid_t)pid, NULL, 0);
            fail_msg("stopped player still running");
        }
    }
    audio_free(audio);
    pop_path();
    capture_test_reset();
    rm_rf(dir);
    free(dir);
}

struct overflow_srv { int listen_fd; int failed; };

static void *overflow_main(void *arg) {
    struct overflow_srv *srv = arg;
    int client = accept(srv->listen_fd, NULL, NULL);
    if (client < 0) { srv->failed = 1; return NULL; }
    char tmp[2048];
    size_t got = 0;
    while (got + 1 < sizeof tmp) {
        ssize_t n = read(client, tmp + got, sizeof tmp - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        tmp[got] = 0;
        if (strstr(tmp, "\r\n\r\n")) break;
    }
    size_t body = (size_t)32 * 1024 * 1024 + 2;
    char head[160];
    int hlen = snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n", body);
    if (hlen < 0 || write(client, head, (size_t)hlen) < 0) srv->failed = 1;
    unsigned char block[65536];
    memset(block, 0, sizeof block);
    size_t left = body;
    while (left && !srv->failed) {
        size_t n = left < sizeof block ? left : sizeof block;
        ssize_t w = write(client, block, n);
        if (w <= 0) { srv->failed = 1; break; }
        left -= (size_t)w;
    }
    close(client);
    return NULL;
}

static void test_http_overflow_respects_tts_cap(void) {
    test_name = "http overflow respects tts cap";
    use_key(NULL);
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(lfd >= 0);
    CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(lfd, 1) == 0);
    socklen_t alen = sizeof addr;
    CHECK(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);
    struct overflow_srv srv = {.listen_fd = lfd};
    pthread_t server;
    CHECK(pthread_create(&server, NULL, overflow_main, &srv) == 0);
    char url[80];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/speech", ntohs(addr.sin_port));
    char *dir = make_tmp();
    audio_config config;
    audio_config_init(&config);
    config.tts_url = url;
    audio *audio = audio_new(dir, &config);
    audio_set_http(audio, NULL, NULL);
    char err[256] = {0};
    atomic_int cancelled = 0;
    int rc = audio_speak(audio, "Hello.", &cancelled, err, sizeof err);
    CHECK(rc == -1);
    CHECK(strstr(err, "exceeds the size limit") != NULL);
    audio_free(audio);
    CHECK(join_ms(server, 5000) == 0);
    close(lfd);
    rm_rf(dir);
    free(dir);
}

int test_audio(void) {
    failures = 0;
    stash_env_key();
    test_chunks();
    test_cancelled_request_holds_engine();
    test_capture_reports_name();
    test_recognition_failure_preserves_recording();
    test_recognition_rejects_invalid_json();
    test_synthesis_failure_does_not_play();
    test_tts_readiness_waits_for_model();
    test_missing_tts_model();
    test_status_does_not_wait_for_health();
    test_recognition_waits_for_startup();
    test_unavailable_model_is_bounded();
    test_cancel_interrupts_readiness();
    test_recognition_sends_vocabulary();
    test_cancelled_recognition_keeps_audio();
    test_cancel_before_http_keeps_gpu_serial();
    test_deepgram_opt_out_and_keyterms();
    test_catalogue_and_preferences();
    test_cloud_only_and_missing_key();
    test_saved_dictation_fails_without_key();
    test_deepgram_request_skips_gpu();
    test_playback_modes_bypass_local_readiness();
    test_cloud_status_hides_local_tts_error();
    test_error_hides_response_and_credentials();
    test_cancelled_deepgram_does_not_send();
    test_speech_request_selects_english();
    test_samantha_switches_reference_at_50_words();
    test_character_choice_persists();
    test_buffered_waits_for_every_chunk();
    test_streaming_prepares_next_chunk();
    test_streaming_cancel_discards_prefetch();
    test_streaming_failure_stops_playback();
    test_cancelling_before_first_streamed_chunk();
    test_cancelling_buffered_discards_before_playback();
    test_later_failure_never_plays_partial();
    test_exit_during_synthesis_leaves_no_buffer();
    test_invalid_later_wav_discards_buffer();
    test_menu_fields_available_on_status();
    test_json_keeps_raw_utf8_and_surrogate_pairs();
    test_wav_rejects_non_pcm_and_short_data_after_junk();
    test_speech_chunks_follow_utf8_boundaries();
    test_preference_close_failure_is_single();
    test_abandoned_worker_drops_cancel_token();
    test_stop_during_player_spawn_discards();
    test_buffered_spawn_receives_private_wav();
    test_closed_player_pipe_does_not_raise_sigpipe();
    test_player_pipe_is_not_inherited_by_capture();
    test_http_overflow_respects_tts_cap();
    restore_env_key();
    if (failures) fprintf(stderr, "%d audio tests failed\n", failures);
    return failures;
}

#ifdef TEST_AUDIO_MAIN
int main(void) {
    return test_audio() == 0 ? 0 : 1;
}
#endif
