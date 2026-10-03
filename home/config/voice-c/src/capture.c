#define _POSIX_C_SOURCE 200809L
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int capture_sample_limit = CAPTURE_RATE * 180;
CaptureSpawnFn capture_spawn_hook;
void *capture_spawn_hook_user;
CaptureReaderStartFn capture_reader_start_hook;
void *capture_reader_start_hook_user;
char capture_open_error[256];

enum {
    LOOKBACK_FRAMES = 2 * CAPTURE_RATE / CAPTURE_FRAME,
    FORCE_FRAMES = 30 * CAPTURE_RATE / CAPTURE_FRAME
};

struct SpeechChunker {
    int frame;
    int silence_frames;
    int force_frames;
    int lookback_frames;
    int16_t *pending;
    size_t len;
    size_t cap;
};

struct PipeWireCapture {
    char *path;
    int wav_fd;
    int header_written;
    uint32_t data_bytes;
    FILE *stderr_file;
    int stderr_fd;
    pid_t pid;
    int stdout_fd;
    int reaped;
    int raw_result;
    int returncode;
    int returncode_set;
    int stopping;
    int has_started;
    double started_at;
    double level;
    double clipped_until;
    char error[256];
    size_t frames;
    SpeechChunker *chunker;
    PcmChunk *chunks;
    size_t chunk_count;
    size_t chunk_cap;
    int progress;
    int ready;
    int done;
    int closed;
    int thread_started;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_mutex_t close_mu;
    pthread_cond_t cv;
    unsigned char odd;
    int has_odd;
    int cleaned;
};

void capture_test_reset(void) {
    capture_spawn_hook = NULL;
    capture_spawn_hook_user = NULL;
    capture_reader_start_hook = NULL;
    capture_reader_start_hook_user = NULL;
    capture_open_error[0] = '\0';
    capture_sample_limit = CAPTURE_RATE * 180;
}

static double local_sqrt(double value) {
    if (value <= 0) return 0;
    double guess = 1;
    while (guess < 1e150 && guess * guess < value) guess *= 2;
    for (int i = 0; i < 40; i++) guess = 0.5 * (guess + value / guess);
    return guess;
}

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

static int wait_flag(pthread_cond_t *cv, pthread_mutex_t *mu, int *flag, double timeout) {
    if (timeout < 0) {
        while (!*flag) pthread_cond_wait(cv, mu);
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    add_seconds(&ts, timeout);
    while (!*flag) {
        int rc = pthread_cond_timedwait(cv, mu, &ts);
        if (rc == ETIMEDOUT) return -1;
        if (rc != 0) return -1;
    }
    return 0;
}

static void set_error_unlocked(PipeWireCapture *c, const char *msg) {
    if (!c->error[0] && msg) snprintf(c->error, sizeof c->error, "%s", msg);
}

static int append_pending(SpeechChunker *c, const int16_t *samples, size_t count) {
    if (count > SIZE_MAX - c->len) return -1;
    if (c->len + count > c->cap) {
        size_t cap = c->cap ? c->cap : 1024;
        while (cap < c->len + count) {
            if (cap > SIZE_MAX / 2) return -1;
            cap *= 2;
        }
        int16_t *next = realloc(c->pending, cap * sizeof *next);
        if (!next) return -1;
        c->pending = next;
        c->cap = cap;
    }
    if (count) memcpy(c->pending + c->len, samples, count * sizeof *samples);
    c->len += count;
    return 0;
}

static int take_chunk(SpeechChunker *c, size_t end_sample, size_t keep, PcmChunk *out) {
    if (end_sample > c->len) end_sample = c->len;
    if (keep < end_sample) keep = end_sample;
    if (keep > c->len) keep = c->len;
    if (end_sample == 0) {
        if (keep) {
            memmove(c->pending, c->pending + keep, (c->len - keep) * sizeof *c->pending);
            c->len -= keep;
        }
        return 0;
    }
    int16_t *copy = malloc(end_sample * sizeof *copy);
    if (!copy) return -1;
    memcpy(copy, c->pending, end_sample * sizeof *copy);
    if (keep < c->len) {
        memmove(c->pending, c->pending + keep, (c->len - keep) * sizeof *c->pending);
    }
    c->len -= keep;
    out->samples = copy;
    out->count = end_sample;
    return 1;
}

static int cut_chunk(SpeechChunker *c, int final, PcmChunk *out) {
    size_t frames = c->frame > 0 ? c->len / (size_t)c->frame : 0;
    if (!frames) {
        if (final && c->len) {
            return take_chunk(c, c->len, c->len, out);
        }
        return 0;
    }
    unsigned char *voiced = malloc(frames);
    if (!voiced) return -1;
    for (size_t index = 0; index < frames; index++) {
        int64_t energy = 0;
        const int16_t *window = c->pending + index * (size_t)c->frame;
        for (int i = 0; i < c->frame; i++) {
            int32_t sample = window[i];
            energy += (int64_t)sample * (int64_t)sample;
        }
        double rms = local_sqrt((double)energy / (double)c->frame);
        voiced[index] = rms >= CAPTURE_VOICED_RMS;
    }
    int seen = 0;
    int silence = 0;
    for (size_t index = 0; index < frames; index++) {
        if (voiced[index]) {
            seen++;
            silence = 0;
            continue;
        }
        silence++;
        if (seen >= CAPTURE_MIN_VOICED_FRAMES && silence >= c->silence_frames) {
            size_t speech_end = (index + 1 - (size_t)silence) * (size_t)c->frame;
            size_t drop_through = (index + 1) * (size_t)c->frame;
            free(voiced);
            return take_chunk(c, speech_end, drop_through, out);
        }
    }
    if (frames >= (size_t)c->force_frames && c->force_frames > 0) {
        int split = c->force_frames;
        int start = split - c->lookback_frames;
        if (start < 0) start = 0;
        for (int index = split - 1; index >= start; index--) {
            if (!voiced[index]) {
                split = index + 1;
                break;
            }
        }
        if (split < CAPTURE_MIN_VOICED_FRAMES) split = c->force_frames;
        free(voiced);
        return take_chunk(c, (size_t)split * (size_t)c->frame, (size_t)split * (size_t)c->frame, out);
    }
    free(voiced);
    if (final) return take_chunk(c, c->len, c->len, out);
    return 0;
}

SpeechChunker *speech_chunker_new(int frame, int silence_frames, int force_frames, int lookback_frames) {
    SpeechChunker *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->frame = frame > 0 ? frame : CAPTURE_FRAME;
    c->silence_frames = silence_frames > 0 ? silence_frames : CAPTURE_SILENCE_FRAMES;
    c->force_frames = force_frames > 0 ? force_frames : FORCE_FRAMES;
    c->lookback_frames = lookback_frames > 0 ? lookback_frames : LOOKBACK_FRAMES;
    return c;
}

void speech_chunker_free(SpeechChunker *chunker) {
    if (!chunker) return;
    free(chunker->pending);
    free(chunker);
}

int speech_chunker_push(SpeechChunker *chunker, const int16_t *samples, size_t count, PcmChunk **out, size_t *out_count) {
    if (!chunker || !out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;
    if (count && append_pending(chunker, samples, count) != 0) return -1;
    size_t cap = 0;
    while (1) {
        PcmChunk chunk = {0};
        int cut = cut_chunk(chunker, 0, &chunk);
        if (cut < 0) {
            pcm_chunks_free(*out, *out_count);
            *out = NULL;
            *out_count = 0;
            return -1;
        }
        if (cut == 0) return 0;
        if (*out_count == cap) {
            size_t next_cap = cap ? cap * 2 : 4;
            PcmChunk *next = realloc(*out, next_cap * sizeof *next);
            if (!next) {
                free(chunk.samples);
                pcm_chunks_free(*out, *out_count);
                *out = NULL;
                *out_count = 0;
                return -1;
            }
            *out = next;
            cap = next_cap;
        }
        (*out)[(*out_count)++] = chunk;
    }
}

int speech_chunker_flush(SpeechChunker *chunker, PcmChunk *out) {
    if (!chunker || !out) return -1;
    out->samples = NULL;
    out->count = 0;
    int cut = cut_chunk(chunker, 1, out);
    if (cut <= 0) {
        out->samples = NULL;
        out->count = 0;
    }
    return cut < 0 ? -1 : 0;
}

void pcm_chunks_free(PcmChunk *chunks, size_t count) {
    if (!chunks) return;
    for (size_t i = 0; i < count; i++) free(chunks[i].samples);
    free(chunks);
}

int capture_spawn_default(char *const *argv, int stderr_fd, CaptureProc *out) {
    int outpipe[2] = {-1, -1};
    int errpipe[2] = {-1, -1};
    if (!argv || !argv[0] || !out) {
        errno = EINVAL;
        return -1;
    }

    if (pipe(outpipe) != 0) return -1;
    if (pipe(errpipe) != 0) {
        close(outpipe[0]);
        close(outpipe[1]);
        return -1;
    }
    fcntl(errpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close(outpipe[0]);
        close(outpipe[1]);
        close(errpipe[0]);
        close(errpipe[1]);
        errno = err;
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        dup2(outpipe[1], STDOUT_FILENO);
        if (stderr_fd >= 0) dup2(stderr_fd, STDERR_FILENO);
        if (outpipe[0] > STDERR_FILENO) close(outpipe[0]);
        if (outpipe[1] > STDERR_FILENO) close(outpipe[1]);
        close(errpipe[0]);
        execvp(argv[0], argv);
        int err = errno;
        if (write(errpipe[1], &err, sizeof err) < 0) _exit(127);
        _exit(127);
    }
    close(outpipe[1]);
    close(errpipe[1]);
    int err = 0;
    size_t got = 0;
    while (got < sizeof err) {
        ssize_t n = read(errpipe[0], (char *)&err + got, sizeof err - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(errpipe[0]);
    if (got > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        close(outpipe[0]);
        errno = err ? err : ENOENT;
        return -1;
    }
    fcntl(outpipe[0], F_SETFD, FD_CLOEXEC);
    out->pid = pid;
    out->stdout_fd = outpipe[0];
    return 0;
}

static int write_all(int fd, const void *buf, size_t count) {
    const unsigned char *p = buf;
    while (count) {
        ssize_t n = write(fd, p, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        count -= (size_t)n;
    }
    return 0;
}

static void put_u16(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void put_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffu);
    p[1] = (unsigned char)((value >> 8) & 0xffu);
    p[2] = (unsigned char)((value >> 16) & 0xffu);
    p[3] = (unsigned char)((value >> 24) & 0xffu);
}

static int write_wav_header(PipeWireCapture *c, uint32_t data_bytes) {
    unsigned char header[44];
    memset(header, 0, sizeof header);
    memcpy(header, "RIFF", 4);
    put_u32(header + 4, 36u + data_bytes);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    put_u32(header + 16, 16);
    put_u16(header + 20, 1);
    put_u16(header + 22, 1);
    put_u32(header + 24, CAPTURE_RATE);
    put_u32(header + 28, CAPTURE_RATE * 2);
    put_u16(header + 32, 2);
    put_u16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put_u32(header + 40, data_bytes);
    return write_all(c->wav_fd, header, sizeof header);
}

static int patch_wav_header(PipeWireCapture *c) {
    unsigned char size[4];
    put_u32(size, 36u + c->data_bytes);
    if (lseek(c->wav_fd, 4, SEEK_SET) < 0 || write_all(c->wav_fd, size, 4) != 0) return -1;
    put_u32(size, c->data_bytes);
    if (lseek(c->wav_fd, 40, SEEK_SET) < 0 || write_all(c->wav_fd, size, 4) != 0) return -1;
    return 0;
}

static int queue_chunk(PipeWireCapture *c, PcmChunk chunk) {
    if (c->chunk_count == c->chunk_cap) {
        size_t cap = c->chunk_cap ? c->chunk_cap * 2 : 4;
        PcmChunk *next = realloc(c->chunks, cap * sizeof *next);
        if (!next) {
            free(chunk.samples);
            return -1;
        }
        c->chunks = next;
        c->chunk_cap = cap;
    }
    c->chunks[c->chunk_count++] = chunk;
    c->progress = 1;
    pthread_cond_broadcast(&c->cv);
    return 0;
}

static void ingest(PipeWireCapture *c, const unsigned char *data, size_t size) {
    if (write_all(c->wav_fd, data, size) != 0) {
        pthread_mutex_lock(&c->mu);
        set_error_unlocked(c, "Microphone capture failed (OSError)");
        pthread_mutex_unlock(&c->mu);
        if (c->pid > 0) kill(c->pid, SIGKILL);
        return;
    }
    c->data_bytes += (uint32_t)size;
    size_t samples_n = size / 2;
    int16_t *samples = malloc(samples_n * sizeof *samples);
    if (!samples) {
        pthread_mutex_lock(&c->mu);
        set_error_unlocked(c, "Microphone capture failed (OSError)");
        pthread_mutex_unlock(&c->mu);
        if (c->pid > 0) kill(c->pid, SIGKILL);
        return;
    }
    int clipped = 0;
    double energy = 0;
    for (size_t i = 0; i < samples_n; i++) {
        int16_t sample = (int16_t)(data[i * 2] | (data[i * 2 + 1] << 8));
        samples[i] = sample;
        if (sample >= 32760 || sample <= -32760) clipped = 1;
        energy += (double)sample * (double)sample;
    }
    double level = samples_n ? local_sqrt(energy / (double)samples_n) / 32768.0 : 0;
    if (level > 1.0) level = 1.0;
    pthread_mutex_lock(&c->mu);
    c->frames += samples_n;
    c->level = level;
    if (clipped) c->clipped_until = mono() + 1.5;
    if (!c->has_started) {
        c->has_started = 1;
        c->started_at = mono();
        c->ready = 1;
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->mu);
    PcmChunk *emitted = NULL;
    size_t emitted_count = 0;
    if (speech_chunker_push(c->chunker, samples, samples_n, &emitted, &emitted_count) != 0) {
        free(samples);
        pthread_mutex_lock(&c->mu);
        set_error_unlocked(c, "Microphone capture failed (OSError)");
        pthread_mutex_unlock(&c->mu);
        if (c->pid > 0) kill(c->pid, SIGKILL);
        return;
    }
    free(samples);
    pthread_mutex_lock(&c->mu);
    for (size_t i = 0; i < emitted_count; i++) {
        if (queue_chunk(c, emitted[i]) != 0) {
            for (size_t j = i + 1; j < emitted_count; j++) free(emitted[j].samples);
            set_error_unlocked(c, "Microphone capture failed (OSError)");
            break;
        }
        emitted[i].samples = NULL;
    }
    pthread_mutex_unlock(&c->mu);
    free(emitted);
}

static int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return 1;
}

static int reap_for(PipeWireCapture *c, double timeout, int *result) {
    pthread_mutex_lock(&c->mu);
    if (c->reaped) {
        *result = c->raw_result;
        pthread_mutex_unlock(&c->mu);
        return 0;
    }
    pthread_mutex_unlock(&c->mu);
    double deadline = mono() + timeout;
    for (;;) {
        int status = 0;
        pid_t got = waitpid(c->pid, &status, WNOHANG);
        if (got == c->pid) {
            pthread_mutex_lock(&c->mu);
            c->reaped = 1;
            c->raw_result = decode_status(status);
            *result = c->raw_result;
            pthread_mutex_unlock(&c->mu);
            return 0;
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                pthread_mutex_lock(&c->mu);
                c->reaped = 1;
                *result = c->raw_result;
                pthread_mutex_unlock(&c->mu);
                return 0;
            }
            return -1;
        }
        if (mono() >= deadline) return 1;
        struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }
}

static void finish_reader(PipeWireCapture *c) {
    if (c->wav_fd >= 0) {
        int failed = 0;
        if (!c->header_written) failed = write_wav_header(c, c->data_bytes);
        else failed = patch_wav_header(c);
        if (failed) {
            pthread_mutex_lock(&c->mu);
            set_error_unlocked(c, "Could not finalize audio WAV");
            pthread_mutex_unlock(&c->mu);
        }
        close(c->wav_fd);
        c->wav_fd = -1;
    }
    if (c->stdout_fd >= 0) {
        close(c->stdout_fd);
        c->stdout_fd = -1;
    }
    int result = 0;
    int waited = reap_for(c, 0.2, &result);
    if (waited != 0) {
        pthread_mutex_lock(&c->mu);
        set_error_unlocked(c, "Microphone stream disconnected");
        pthread_mutex_unlock(&c->mu);
        if (c->pid > 0 && !c->reaped) kill(c->pid, SIGKILL);
        if (reap_for(c, 0.5, &result) != 0) result = c->raw_result;
    }
    struct stat st;
    int stderr_empty = 0;
    if (c->stderr_fd >= 0 && fstat(c->stderr_fd, &st) == 0 && st.st_size == 0) stderr_empty = 1;
    pthread_mutex_lock(&c->mu);
    if (result == 1 && c->has_started && !c->error[0]
        && (c->stopping || c->frames >= (size_t)capture_sample_limit)
        && stderr_empty) {
        result = 0;
    }
    c->returncode = result;
    c->returncode_set = 1;
    int stopped = c->stopping && result < 0;
    if (result && !stopped) {
        char msg[80];
        snprintf(msg, sizeof msg, "Microphone capture exited with status %d", result);
        set_error_unlocked(c, msg);
    }
    if (!c->has_started) set_error_unlocked(c, "Microphone stopped before producing samples");
    PcmChunk remainder = {0};
    if (c->chunker && speech_chunker_flush(c->chunker, &remainder) == 0 && remainder.samples) {
        queue_chunk(c, remainder);
    }
    c->progress = 1;
    c->done = 1;
    c->ready = 1;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

static void *reader_main(void *arg) {
    PipeWireCapture *c = arg;
    unsigned char buf[8192];
    unsigned char joined[8193];
    double last_samples = mono();
    while (!c->cleaned) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(c->stdout_fd, &fds);
        struct timeval tv = {0, 100000};
        int ready = select(c->stdout_fd + 1, &fds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            pthread_mutex_lock(&c->mu);
            set_error_unlocked(c, "Microphone capture failed (OSError)");
            pthread_mutex_unlock(&c->mu);
            if (c->pid > 0) kill(c->pid, SIGKILL);
            break;
        }
        if (ready == 0) {
            int status = 0;
            pid_t got = waitpid(c->pid, &status, WNOHANG);
            if (got == c->pid) {
                pthread_mutex_lock(&c->mu);
                c->reaped = 1;
                c->raw_result = decode_status(status);
                pthread_mutex_unlock(&c->mu);
                break;
            }
            pthread_mutex_lock(&c->mu);
            int started = c->has_started;
            int stopping = c->stopping;
            pthread_mutex_unlock(&c->mu);
            if (started && !stopping && mono() - last_samples > 3.0) {
                pthread_mutex_lock(&c->mu);
                set_error_unlocked(c, "Microphone audio stream stalled");
                pthread_mutex_unlock(&c->mu);
                if (c->pid > 0) kill(c->pid, SIGKILL);
                break;
            }
            continue;
        }
        ssize_t n = read(c->stdout_fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            pthread_mutex_lock(&c->mu);
            set_error_unlocked(c, "Microphone capture failed (OSError)");
            pthread_mutex_unlock(&c->mu);
            if (c->pid > 0) kill(c->pid, SIGKILL);
            break;
        }
        if (n == 0) break;
        size_t total = 0;
        if (c->has_odd) joined[total++] = c->odd;
        if ((size_t)n > sizeof joined - total) n = (ssize_t)(sizeof joined - total);
        memcpy(joined + total, buf, (size_t)n);
        total += (size_t)n;
        size_t even = total - (total % 2);
        c->has_odd = total % 2;
        if (c->has_odd) c->odd = joined[even];
        if (!even) continue;
        if (!c->header_written) {
            if (write_wav_header(c, 0) != 0) {
                pthread_mutex_lock(&c->mu);
                set_error_unlocked(c, "Microphone capture failed (OSError)");
                pthread_mutex_unlock(&c->mu);
                if (c->pid > 0) kill(c->pid, SIGKILL);
                break;
            }
            c->header_written = 1;
        }
        last_samples = mono();
        ingest(c, joined, even);
        pthread_mutex_lock(&c->mu);
        int failed = c->error[0] != '\0';
        pthread_mutex_unlock(&c->mu);
        if (failed) break;
    }
    finish_reader(c);
    return NULL;
}

static void cleanup_failed_open(PipeWireCapture *c, int kill_child) {
    if (kill_child && c->pid > 0) {
        kill(c->pid, SIGKILL);
        int status = 0;
        double deadline = mono() + 0.5;
        while (mono() < deadline) {
            pid_t got = waitpid(c->pid, &status, WNOHANG);
            if (got == c->pid || (got < 0 && errno != EINTR)) break;
            struct timespec pause = {0, 10000000L};
            nanosleep(&pause, NULL);
        }
        c->reaped = 1;
    }
    if (c->stdout_fd >= 0) close(c->stdout_fd);
    if (c->wav_fd >= 0) {
        close(c->wav_fd);
        if (c->path) unlink(c->path);
    }
    if (c->stderr_file) fclose(c->stderr_file);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    pthread_mutex_destroy(&c->close_mu);
    speech_chunker_free(c->chunker);
    free(c->path);
    free(c);
}

PipeWireCapture *capture_open(const char *path, char *const *command, const char *target) {
    capture_open_error[0] = '\0';
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    PipeWireCapture *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->wav_fd = -1;
    c->stdout_fd = -1;
    c->stderr_fd = -1;
    c->pid = -1;
    pthread_mutex_init(&c->mu, NULL);
    pthread_mutex_init(&c->close_mu, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&c->cv, &attr);
    pthread_condattr_destroy(&attr);
    c->chunker = speech_chunker_new(CAPTURE_FRAME, CAPTURE_SILENCE_FRAMES, FORCE_FRAMES, LOOKBACK_FRAMES);
    c->path = strdup(path);
    char errname[] = "/tmp/voice-cap-err-XXXXXX";
    int err_fd = mkstemp(errname);
    if (err_fd >= 0) {
        unlink(errname);
        c->stderr_file = fdopen(err_fd, "r+");
    }
    if (!c->chunker || !c->path || !c->stderr_file) {
        if (err_fd >= 0 && !c->stderr_file) close(err_fd);
        cleanup_failed_open(c, 0);
        errno = ENOMEM;
        return NULL;
    }
    c->stderr_fd = fileno(c->stderr_file);
    c->wav_fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (c->wav_fd < 0) {
        int err = errno;
        cleanup_failed_open(c, 0);
        errno = err;
        return NULL;
    }
    char sample_arg[64];
    snprintf(sample_arg, sizeof sample_arg, "--sample-count=%d", capture_sample_limit);
    char *built[10];
    char *const *argv = command;
    if (!argv) {
        int n = 0;
        built[n++] = "pw-record";
        built[n++] = "--raw";
        built[n++] = "--rate=16000";
        built[n++] = "--channels=1";
        built[n++] = "--format=s16";
        built[n++] = sample_arg;
        if (target && target[0]) {
            built[n++] = "--target";
            built[n++] = (char *)target;
        }
        built[n++] = "-";
        built[n] = NULL;
        argv = built;
    }
    CaptureProc proc = {0};
    int spawned;
    if (capture_spawn_hook) {
        spawned = capture_spawn_hook(argv, c->stderr_fd, &proc, capture_spawn_hook_user);
    } else {
        spawned = capture_spawn_default(argv, c->stderr_fd, &proc);
    }
    if (spawned != 0) {
        int err = errno;
        cleanup_failed_open(c, 0);
        errno = err ? err : ENOENT;
        return NULL;
    }
    c->pid = proc.pid;
    c->stdout_fd = proc.stdout_fd;
    if (capture_reader_start_hook && capture_reader_start_hook(capture_reader_start_hook_user) != 0) {
        if (!capture_open_error[0]) {
            snprintf(capture_open_error, sizeof capture_open_error, "reader start failed");
        }
        cleanup_failed_open(c, 1);
        errno = EAGAIN;
        return NULL;
    }
    if (pthread_create(&c->thread, NULL, reader_main, c) != 0) {
        snprintf(capture_open_error, sizeof capture_open_error, "Microphone capture failed (EAGAIN)");
        cleanup_failed_open(c, 1);
        errno = EAGAIN;
        return NULL;
    }
    c->thread_started = 1;
    return c;
}

void capture_send_signal(PipeWireCapture *capture, int sig) {
    if (!capture) return;
    pthread_mutex_lock(&capture->mu);
    capture->stopping = 1;
    pid_t pid = capture->pid;
    int reaped = capture->reaped;
    pthread_mutex_unlock(&capture->mu);
    if (!reaped && pid > 0) {
        if (kill(pid, sig) < 0 && errno != ESRCH) return;
    }
}

void capture_close(PipeWireCapture *capture) {
    if (!capture) return;
    pthread_mutex_lock(&capture->close_mu);
    if (capture->closed) {
        pthread_mutex_unlock(&capture->close_mu);
        return;
    }
    int sigs[3] = {SIGINT, SIGTERM, SIGKILL};
    for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&capture->mu);
        int done = capture->done;
        pthread_mutex_unlock(&capture->mu);
        if (done) break;
        capture_send_signal(capture, sigs[i]);
        pthread_mutex_lock(&capture->mu);
        wait_flag(&capture->cv, &capture->mu, &capture->done, 0.3);
        pthread_mutex_unlock(&capture->mu);
    }
    if (capture->thread_started) {
        pthread_join(capture->thread, NULL);
        capture->thread_started = 0;
    }
    if (capture->stderr_file) {
        fclose(capture->stderr_file);
        capture->stderr_file = NULL;
        capture->stderr_fd = -1;
    }
    capture->closed = 1;
    pthread_mutex_unlock(&capture->close_mu);
}

void capture_free(PipeWireCapture *capture) {
    if (!capture) return;
    capture_close(capture);
    pthread_cond_destroy(&capture->cv);
    pthread_mutex_destroy(&capture->mu);
    pthread_mutex_destroy(&capture->close_mu);
    speech_chunker_free(capture->chunker);
    pcm_chunks_free(capture->chunks, capture->chunk_count);
    if (capture->wav_fd >= 0) close(capture->wav_fd);
    if (capture->stdout_fd >= 0) close(capture->stdout_fd);
    free(capture->path);
    free(capture);
}

int capture_wait_ready(PipeWireCapture *capture, double timeout) {
    if (!capture) return -1;
    if (timeout < 0) timeout = 5.0;
    pthread_mutex_lock(&capture->mu);
    int rc = wait_flag(&capture->cv, &capture->mu, &capture->ready, timeout);
    if (rc != 0) {
        set_error_unlocked(capture, "Microphone produced no audio samples before timeout");
        pthread_mutex_unlock(&capture->mu);
        capture_close(capture);
        return -1;
    }
    int bad = capture->error[0] != '\0';
    pthread_mutex_unlock(&capture->mu);
    return bad ? -1 : 0;
}

int capture_wait(PipeWireCapture *capture, double timeout, int *code) {
    if (!capture) return -1;
    pthread_mutex_lock(&capture->mu);
    int rc = wait_flag(&capture->cv, &capture->mu, &capture->done, timeout);
    int result = capture->returncode;
    pthread_mutex_unlock(&capture->mu);
    if (rc != 0) return -1;
    if (code) *code = result;
    return 0;
}

int capture_poll(PipeWireCapture *capture, int *code) {
    if (!capture) return 1;
    pthread_mutex_lock(&capture->mu);
    int done = capture->done;
    int result = capture->returncode;
    pthread_mutex_unlock(&capture->mu);
    if (!done) return 1;
    if (code) *code = result;
    return 0;
}

int capture_drain_chunks(PipeWireCapture *capture, PcmChunk **out, size_t *count) {
    if (!capture || !out || !count) return -1;
    pthread_mutex_lock(&capture->mu);
    *out = capture->chunks;
    *count = capture->chunk_count;
    capture->chunks = NULL;
    capture->chunk_count = 0;
    capture->chunk_cap = 0;
    if (*count == 0 && !capture->done) capture->progress = 0;
    pthread_mutex_unlock(&capture->mu);
    return 0;
}

int capture_wait_progress(PipeWireCapture *capture, double timeout) {
    if (!capture) return 0;
    pthread_mutex_lock(&capture->mu);
    int rc = 0;
    if (!capture->progress) rc = wait_flag(&capture->cv, &capture->mu, &capture->progress, timeout);
    int got = capture->progress;
    pthread_mutex_unlock(&capture->mu);
    (void)rc;
    return got;
}

const char *capture_error(PipeWireCapture *capture) {
    if (!capture || !capture->error[0]) return NULL;
    return capture->error;
}

double capture_level(PipeWireCapture *capture) {
    if (!capture) return 0;
    pthread_mutex_lock(&capture->mu);
    double level = capture->level;
    pthread_mutex_unlock(&capture->mu);
    return level;
}

int capture_clipping(PipeWireCapture *capture) {
    if (!capture) return 0;
    pthread_mutex_lock(&capture->mu);
    double until = capture->clipped_until;
    pthread_mutex_unlock(&capture->mu);
    return mono() < until;
}

int capture_has_started(PipeWireCapture *capture) {
    if (!capture) return 0;
    pthread_mutex_lock(&capture->mu);
    int started = capture->has_started;
    pthread_mutex_unlock(&capture->mu);
    return started;
}

double capture_started_at(PipeWireCapture *capture) {
    if (!capture) return 0;
    pthread_mutex_lock(&capture->mu);
    double at = capture->started_at;
    pthread_mutex_unlock(&capture->mu);
    return at;
}
