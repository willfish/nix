#define _POSIX_C_SOURCE 200809L
#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static char dir_template[] = "/tmp/voice-cap-XXXXXX";
static char *dir;
static char path[256];

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fail(const char *name, const char *detail) {
    fprintf(stderr, "FAIL %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
    failures++;
}

static void setup(void) {
    capture_test_reset();
    dir = mkdtemp(dir_template);
    if (!dir) {
        fail("setup", "mkdtemp");
        path[0] = '\0';
        return;
    }
    snprintf(path, sizeof path, "%s/capture.wav", dir);
}

static void teardown(void) {
    if (path[0]) unlink(path);
    if (dir) rmdir(dir);
    strcpy(dir_template, "/tmp/voice-cap-XXXXXX");
    dir = NULL;
    path[0] = '\0';
    capture_test_reset();
}

static PipeWireCapture *open_script(const char *script) {
    char code[8192];
    snprintf(code, sizeof code,
        "const fs=require('node:fs');"
        "const pcm=(n,v=0)=>{const b=Buffer.alloc(n*2);for(let i=0;i<n;i++)b.writeInt16LE(v,i*2);return b;};"
        "const write=b=>fs.writeSync(1,b);"
        "const sleep=ms=>new Promise(r=>setTimeout(r,ms));"
        "(async()=>{%s})().catch(e=>{console.error(e);process.exitCode=1;});", script);
    char *argv[] = {"node", "-e", code, NULL};
    return capture_open(path, argv, NULL);
}

static char **preferred_recorded;
static int preferred_recorded_n;

static int record_preferred_spawn(char *const *argv, int stderr_fd, CaptureProc *out, void *user) {
    (void)user;
    for (int i = 0; i < preferred_recorded_n; i++) free(preferred_recorded[i]);
    free(preferred_recorded);
    preferred_recorded = NULL;
    preferred_recorded_n = 0;
    while (argv[preferred_recorded_n]) preferred_recorded_n++;
    preferred_recorded = calloc((size_t)preferred_recorded_n, sizeof *preferred_recorded);
    for (int i = 0; i < preferred_recorded_n; i++) preferred_recorded[i] = strdup(argv[i]);
    char *producer[] = {"node", "-e", "require('node:fs').writeSync(1,Buffer.alloc(320))", NULL};
    return capture_spawn_default(producer, stderr_fd, out);
}

static int near(double got, double want) {
    double delta = got - want;
    if (delta < 0) delta = -delta;
    return delta < 0.0005;
}

static int exited_with(const char *error, int code) {
    char buf[80];
    snprintf(buf, sizeof buf, "Microphone capture exited with status %d", code);
    return error && strcmp(error, buf) == 0;
}

static int read_wav(int *rate, int *channels, int *width, unsigned char **frames, size_t *count) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    unsigned char header[44];
    if (fread(header, 1, sizeof header, file) != sizeof header) {
        fclose(file);
        return -1;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(file);
        return -1;
    }
    *channels = header[22] | (header[23] << 8);
    *rate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    *width = (header[34] | (header[35] << 8)) / 8;
    size_t bytes = (size_t)header[40] | ((size_t)header[41] << 8) | ((size_t)header[42] << 16) | ((size_t)header[43] << 24);
    unsigned char *data = malloc(bytes ? bytes : 1);
    if (!data || fread(data, 1, bytes, file) != bytes) {
        free(data);
        fclose(file);
        return -1;
    }
    fclose(file);
    *frames = data;
    *count = *width ? bytes / (size_t)*width : 0;
    return 0;
}

static void test_preferred_target(void) {
    capture_spawn_hook = record_preferred_spawn;
    PipeWireCapture *capture = capture_open(path, NULL, "alsa_input.usb-microphone");
    if (!capture || capture_wait_ready(capture, 1) != 0) {
        fail("preferred target", "not ready");
    } else {
        int code = -1;
        if (capture_wait(capture, 1, &code) != 0 || code != 0) fail("preferred target", "wait");
        int at = -1;
        for (int i = 0; i < preferred_recorded_n; i++) {
            if (strcmp(preferred_recorded[i], "--target") == 0) at = i;
        }
        if (at < 0 || at + 1 >= preferred_recorded_n || strcmp(preferred_recorded[at + 1], "alsa_input.usb-microphone") != 0) {
            fail("preferred target", "argv");
        }
        const char *want[] = {
            "pw-record", "--raw", "--rate=16000", "--channels=1", "--format=s16",
            "--sample-count=2880000", "--target", "alsa_input.usb-microphone", "-",
        };
        if (preferred_recorded_n != 9) fail("preferred target", "argc");
        for (int i = 0; i < preferred_recorded_n && i < 9; i++) {
            if (!preferred_recorded[i] || strcmp(preferred_recorded[i], want[i]) != 0)
                fail("preferred target", preferred_recorded[i] ? preferred_recorded[i] : "null");
        }
    }
    capture_free(capture);
    for (int i = 0; i < preferred_recorded_n; i++) free(preferred_recorded[i]);
    free(preferred_recorded);
    preferred_recorded = NULL;
    preferred_recorded_n = 0;
}

static void test_chunker(void) {
    SpeechChunker *chunker = speech_chunker_new(320, 5, 20, 10);
    int16_t *voiced = malloc(8 * 320 * sizeof *voiced);
    for (int i = 0; i < 8 * 320; i++) voiced[i] = 4000;
    PcmChunk *out = NULL;
    size_t count = 0;
    if (speech_chunker_push(chunker, voiced, 8 * 320, &out, &count) != 0 || count != 0) {
        fail("chunker pause", "early emit");
    }
    pcm_chunks_free(out, count);
    int16_t *silence = calloc(5 * 320, sizeof *silence);
    if (speech_chunker_push(chunker, silence, 5 * 320, &out, &count) != 0 || count != 1 || out[0].count != 8 * 320) {
        fail("chunker pause", "split length");
    }
    pcm_chunks_free(out, count);
    PcmChunk flushed = {0};
    if (speech_chunker_flush(chunker, &flushed) != 0 || flushed.samples) fail("chunker pause", "flush");
    free(flushed.samples);
    speech_chunker_free(chunker);

    SpeechChunker *forced = speech_chunker_new(320, 50, 10, 4);
    int16_t *long_voiced = malloc(12 * 320 * sizeof *long_voiced);
    for (int i = 0; i < 12 * 320; i++) long_voiced[i] = 4000;
    if (speech_chunker_push(forced, long_voiced, 12 * 320, &out, &count) != 0 || count != 1 || out[0].count != 10 * 320) {
        fail("chunker force", "split length");
    }
    pcm_chunks_free(out, count);
    if (speech_chunker_flush(forced, &flushed) != 0 || !flushed.samples || flushed.count != 2 * 320) {
        fail("chunker force", "remainder");
    }
    free(flushed.samples);
    speech_chunker_free(forced);
    free(voiced);
    free(silence);
    free(long_voiced);

    SpeechChunker *partial = speech_chunker_new(320, 30, 1500, 100);
    int16_t tail[100];
    for (int i = 0; i < 100; i++) tail[i] = 4000;
    if (speech_chunker_push(partial, tail, 100, &out, &count) != 0 || count != 0) fail("partial frame", "emitted");
    pcm_chunks_free(out, count);
    if (speech_chunker_flush(partial, &flushed) != 0 || !flushed.samples || flushed.count != 100) {
        fail("partial frame", "dropped");
    }
    free(flushed.samples);
    speech_chunker_free(partial);
}

static void test_pause_emits_before_end(void) {
    PipeWireCapture *capture = open_script(
        "process.on('SIGINT',()=>process.exit(0));"
        "write(pcm(3200,8192));await sleep(50);"
        "write(pcm(12800));await sleep(50);"
        "write(pcm(3200,8192));await sleep(10000);");
    if (!capture || capture_wait_ready(capture, 2) != 0) {
        fail("pause chunk", "not ready");
        capture_free(capture);
        return;
    }
    PcmChunk *chunks = NULL;
    size_t count = 0;
    double deadline = mono() + 2;
    while (mono() < deadline) {
        PcmChunk *more = NULL;
        size_t more_count = 0;
        capture_drain_chunks(capture, &more, &more_count);
        if (more_count) {
            chunks = more;
            count = more_count;
            break;
        }
        pcm_chunks_free(more, more_count);
        capture_wait_progress(capture, 0.05);
    }
    int code = 0;
    if (!chunks || capture_poll(capture, &code) == 0) fail("pause chunk", "missing or already done");
    else if (chunks[0].count != 10 * 320) fail("pause chunk", "length");
    pcm_chunks_free(chunks, count);
    capture_close(capture);
    if (capture_wait(capture, 1, &code) != 0 || code != 0) fail("pause chunk", "close wait");
    capture_drain_chunks(capture, &chunks, &count);
    if (!count || chunks[0].count != 10 * 320) {
        fprintf(stderr, "  remainder count=%zu len=%zu\n", count, count ? chunks[0].count : 0);
        fail("pause chunk", "remainder");
    }
    pcm_chunks_free(chunks, count);
    capture_free(capture);
}

static void test_clipping(void) {
    PipeWireCapture *capture = open_script("write(pcm(320,32767));");
    int code = -1;
    if (!capture || capture_wait_ready(capture, 1) != 0 || capture_wait(capture, 1, &code) != 0 || code != 0) {
        fail("clipping", "wait");
    } else if (!capture_clipping(capture)) {
        fail("clipping", "not latched");
    }
    capture_free(capture);
}

static void test_ready_finalizes_wav(void) {
    PipeWireCapture *capture = open_script(
        "process.on('SIGINT',()=>process.exit(0));"
        "write(pcm(320,8192));await sleep(10000);");
    if (!capture || capture_wait_ready(capture, 2) != 0) {
        fail("ready wav", "not ready");
        capture_free(capture);
        return;
    }
    if (!(capture_started_at(capture) > 0)) fail("ready wav", "started_at");
    if (!near(capture_level(capture), 0.25)) fail("ready wav", "level");
    int code = 0;
    if (capture_poll(capture, &code) == 0) fail("ready wav", "finished early");
    capture_send_signal(capture, SIGINT);
    if (capture_wait(capture, 2, &code) != 0 || code != 0) fail("ready wav", "wait");
    if (capture_poll(capture, &code) != 0 || code != 0) fail("ready wav", "poll");
    if (capture_error(capture)) fail("ready wav", capture_error(capture));
    int rate = 0, channels = 0, width = 0;
    unsigned char *frames = NULL;
    size_t count = 0;
    if (read_wav(&rate, &channels, &width, &frames, &count) != 0 || rate != 16000 || channels != 1
        || width != 2 || count != 320) {
        fail("ready wav", "format");
    } else {
        for (size_t i = 0; i < 320; i++) {
            if (frames[i * 2] != 0x00 || frames[i * 2 + 1] != 0x20) {
                fail("ready wav", "pcm");
                break;
            }
        }
    }
    free(frames);
    capture_free(capture);
}

static void test_no_samples(void) {
    PipeWireCapture *capture = open_script("");
    if (!capture || capture_wait_ready(capture, 1) == 0) fail("no samples", "ready");
    else if (!capture_error(capture) || !strstr(capture_error(capture), "samples")) fail("no samples", "error");
    int code = -1;
    if (capture_wait(capture, 1, &code) != 0 || code != 0) fail("no samples", "wait");
    if (capture_has_started(capture)) fail("no samples", "started");
    capture_free(capture);
}

static void test_producer_timeout_reaped(void) {
    PipeWireCapture *capture = open_script("await sleep(10000);");
    if (!capture || capture_wait_ready(capture, 0.1) == 0) fail("startup timeout", "ready");
    else if (!capture_error(capture) || !strstr(capture_error(capture), "samples")) fail("startup timeout", "error");
    int code = 0;
    if (capture_poll(capture, &code) != 0) fail("startup timeout", "not reaped");
    if (capture_has_started(capture)) fail("startup timeout", "started");
    capture_free(capture);
}

static void test_delayed_samples(void) {
    PipeWireCapture *capture = open_script(
        "await sleep(200);write(pcm(160));");
    double started = mono();
    if (!capture || capture_wait_ready(capture, 2) != 0) fail("delayed", "not ready");
    else if (capture_started_at(capture) - started < 0.15) {
        fprintf(stderr, "  delayed delta=%f\n", capture_started_at(capture) - started);
        fail("delayed", "too soon");
    }
    int code = -1;
    if (capture && (capture_wait(capture, 1, &code) != 0 || code != 0)) fail("delayed", "wait");
    capture_free(capture);
}

static void test_odd_byte_boundary(void) {
    PipeWireCapture *capture = open_script(
        "write(Buffer.from([0]));await sleep(100);"
        "write(Buffer.concat([Buffer.from([32]),pcm(99,8192)]));");
    double started = mono();
    if (!capture || capture_wait_ready(capture, 1) != 0) fail("odd byte", "not ready");
    else if (capture_started_at(capture) - started < 0.08) {
        fprintf(stderr, "  odd delta=%f\n", capture_started_at(capture) - started);
        fail("odd byte", "too soon");
    }
    int code = -1;
    if (capture && (capture_wait(capture, 1, &code) != 0 || code != 0)) fail("odd byte", "wait");
    if (capture && !near(capture_level(capture), 0.25)) fail("odd byte", "level");
    int rate = 0, channels = 0, width = 0;
    unsigned char *frames = NULL;
    size_t count = 0;
    if (read_wav(&rate, &channels, &width, &frames, &count) != 0 || count != 100) fail("odd byte", "frames");
    else {
        for (size_t i = 0; i < 100; i++) {
            if (frames[i * 2] != 0x00 || frames[i * 2 + 1] != 0x20) {
                fail("odd byte", "pcm");
                break;
            }
        }
    }
    free(frames);
    capture_free(capture);
}

static void test_abnormal_exit(void) {
    PipeWireCapture *capture = open_script(
        "write(pcm(160));await sleep(100);process.exit(7);");
    int code = 0;
    if (!capture || capture_wait_ready(capture, 1) != 0) fail("exit 7", "not ready");
    if (capture && (capture_wait(capture, 1, &code) != 0 || code != 7)) fail("exit 7", "code");
    if (!exited_with(capture ? capture_error(capture) : NULL, 7)) fail("exit 7", capture ? capture_error(capture) : "null");
    capture_free(capture);
}

static void test_exit_one_after_stop(void) {
    PipeWireCapture *capture = open_script(
        "process.on('SIGINT',()=>process.exit(1));"
        "write(pcm(320));await sleep(10000);");
    int code = -1;
    if (!capture || capture_wait_ready(capture, 1) != 0) fail("exit 1 stop", "not ready");
    capture_close(capture);
    if (capture && (capture_wait(capture, 1, &code) != 0 || code != 0)) fail("exit 1 stop", "wait");
    if (capture && (capture_poll(capture, &code) != 0 || code != 0)) fail("exit 1 stop", "poll");
    if (capture && capture_error(capture)) fail("exit 1 stop", capture_error(capture));
    int rate = 0, channels = 0, width = 0;
    unsigned char *frames = NULL;
    size_t count = 0;
    if (read_wav(&rate, &channels, &width, &frames, &count) != 0 || count != 320) fail("exit 1 stop", "frames");
    free(frames);
    capture_free(capture);
}

static void test_exit_one_without_stop(void) {
    PipeWireCapture *capture = open_script("write(pcm(320));process.exit(1);");
    int code = 0;
    if (!capture || capture_wait(capture, 1, &code) != 0 || code != 1) fail("exit 1", "code");
    if (!exited_with(capture ? capture_error(capture) : NULL, 1)) fail("exit 1", capture ? capture_error(capture) : "null");
    capture_free(capture);
}

static void test_stderr_not_hidden(void) {
    PipeWireCapture *capture = open_script(
        "process.on('SIGINT',()=>process.exit(1));"
        "fs.writeSync(2,'error: connection lost\\n');"
        "write(pcm(320));await sleep(10000);");
    int code = 0;
    if (!capture || capture_wait_ready(capture, 1) != 0) fail("stderr", "not ready");
    capture_close(capture);
    if (capture && (capture_poll(capture, &code) != 0 || code != 1)) {
        fprintf(stderr, "  stderr poll done=%d code=%d err=%s\n", capture_poll(capture, &code), code, capture_error(capture) ? capture_error(capture) : "null");
        fail("stderr", "poll");
    }
    if (!exited_with(capture ? capture_error(capture) : NULL, 1)) fail("stderr", capture && capture_error(capture) ? capture_error(capture) : "null");
    capture_free(capture);
}

static void test_sample_limit_exit_one(void) {
    int saved = capture_sample_limit;
    capture_sample_limit = 320;
    PipeWireCapture *capture = open_script("write(pcm(320));process.exit(1);");
    int code = -1;
    if (!capture || capture_wait(capture, 1, &code) != 0 || code != 0) fail("sample limit", "code");
    if (capture && capture_error(capture)) fail("sample limit", capture_error(capture));
    capture_free(capture);
    capture_sample_limit = saved;
}

static void test_stall(void) {
    PipeWireCapture *capture = open_script(
        "write(pcm(160));await sleep(10000);");
    int code = 0;
    if (!capture || capture_wait_ready(capture, 1) != 0) fail("stall", "not ready");
    if (capture && capture_wait(capture, 4, &code) != 0) fail("stall", "not stopped");
    else if (code == 0) {
        fprintf(stderr, "  stall code 0 err=%s\n", capture && capture_error(capture) ? capture_error(capture) : "null");
        fail("stall", "success code");
    }
    if (!capture || !capture_error(capture) || !strstr(capture_error(capture), "stalled")) {
        fail("stall", capture && capture_error(capture) ? capture_error(capture) : "null");
    }
    capture_free(capture);
}

static void test_disconnect(void) {
    PipeWireCapture *capture = open_script(
        "write(pcm(160));fs.closeSync(1);await sleep(10000);");
    int code = 0;
    if (!capture || capture_wait_ready(capture, 1) != 0) fail("disconnect", "not ready");
    if (capture && capture_wait(capture, 1, &code) != 0) fail("disconnect", "not reaped");
    else if (code == 0) fail("disconnect", "success code");
    if (!capture || !capture_error(capture) || !strstr(capture_error(capture), "disconnected")) {
        fail("disconnect", capture && capture_error(capture) ? capture_error(capture) : "null");
    }
    capture_free(capture);
}

static void test_close_escalates(void) {
    PipeWireCapture *capture = open_script(
        "process.on('SIGINT',()=>{});process.on('SIGTERM',()=>{});"
        "write(pcm(160));await sleep(10000);");
    if (!capture || capture_wait_ready(capture, 1) != 0) {
        fail("escalate", "not ready");
        capture_free(capture);
        return;
    }
    double started = mono();
    capture_close(capture);
    if (mono() - started >= 2) fail("escalate", "too slow");
    int code = 0;
    if (capture_poll(capture, &code) != 0 || code != -SIGKILL) {
        fprintf(stderr, "  escalate poll=%d code=%d err=%s\n", capture_poll(capture, &code), code, capture_error(capture) ? capture_error(capture) : "null");
        fail("escalate", "signal");
    }
    capture_close(capture);
    capture_send_signal(capture, SIGINT);
    capture_free(capture);
}

static void test_constructor_failure_unlinks(void) {
    char missing[256];
    snprintf(missing, sizeof missing, "%s/absent", dir);
    char *argv[] = {missing, NULL};
    PipeWireCapture *capture = capture_open(path, argv, NULL);
    if (capture) fail("constructor", "opened");
    struct stat st;
    if (stat(path, &st) == 0) fail("constructor", "wav remains");
    capture_free(capture);
}

static pid_t spawned_pid;

static int record_pid(char *const *argv, int stderr_fd, CaptureProc *out, void *user) {
    (void)user;
    int rc = capture_spawn_default(argv, stderr_fd, out);
    if (rc == 0) spawned_pid = out->pid;
    return rc;
}

static int fail_reader(void *user) {
    (void)user;
    snprintf(capture_open_error, sizeof capture_open_error, "Threads exhausted");
    return -1;
}

static void test_reader_start_failure(void) {
    spawned_pid = 0;
    capture_spawn_hook = record_pid;
    capture_reader_start_hook = fail_reader;
    PipeWireCapture *capture = open_script("await sleep(10000);");
    if (capture) fail("reader start", "opened");
    struct stat st;
    if (stat(path, &st) == 0) fail("reader start", "wav remains");
    if (!spawned_pid || kill(spawned_pid, 0) == 0) fail("reader start", "still running");
    if (!strstr(capture_open_error, "exhausted")) fail("reader start", capture_open_error);
    if (spawned_pid > 0) {
        kill(spawned_pid, SIGKILL);
        waitpid(spawned_pid, NULL, 0);
    }
}

static void test_cancel_before_ready(void) {
    PipeWireCapture *capture = open_script("await sleep(10000);");
    if (!capture) {
        fail("cancel", "open");
        return;
    }
    capture_close(capture);
    int code = 0;
    if (capture_poll(capture, &code) != 0) fail("cancel", "not reaped");
    if (capture_wait_ready(capture, 0.1) == 0) fail("cancel", "ready");
    else if (!capture_error(capture) || !strstr(capture_error(capture), "samples")) fail("cancel", "error");
    capture_free(capture);
}

static void test_spawn_stdout_cloexec(void) {
    char *argv[] = {"node", "-e", "require('node:fs').writeSync(1,Buffer.alloc(2))", NULL};
    CaptureProc proc = {0};
    if (capture_spawn_default(argv, -1, &proc) != 0) {
        fail("cloexec", "spawn");
        return;
    }
    int flags = fcntl(proc.stdout_fd, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0) fail("cloexec", "stdout");
    if (proc.pid > 0) kill(proc.pid, SIGKILL);
    waitpid(proc.pid, NULL, 0);
    if (proc.stdout_fd >= 0) close(proc.stdout_fd);
}

static void test_high_fd_read(void) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
        fprintf(stderr, "SKIP high fd: getrlimit\n");
        return;
    }
    struct rlimit raised = limit;
    rlim_t want = (rlim_t)FD_SETSIZE + 64;
    if (raised.rlim_max < want) {
        fprintf(stderr, "SKIP high fd: rlimit max\n");
        return;
    }
    raised.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &raised) != 0) {
        fprintf(stderr, "SKIP high fd: setrlimit\n");
        return;
    }
    int *held = calloc((size_t)FD_SETSIZE + 8, sizeof *held);
    int nheld = 0;
    int reached = 0;
    if (held) {
        while (nheld < FD_SETSIZE + 8) {
            int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fd < 0) break;
            if (fd >= FD_SETSIZE) {
                close(fd);
                reached = 1;
                break;
            }
            held[nheld++] = fd;
        }
    }
    if (!reached) {
        fprintf(stderr, "SKIP high fd: could not fill the select set\n");
    } else {
        PipeWireCapture *capture = open_script("write(pcm(320));");
        if (!capture || capture_wait_ready(capture, 2) != 0) fail("high fd", "not ready");
        else {
            int code = -1;
            if (capture_wait(capture, 2, &code) != 0 || code != 0) fail("high fd", "wait");
        }
        capture_free(capture);
    }
    for (int i = 0; i < nheld; i++) close(held[i]);
    free(held);
    setrlimit(RLIMIT_NOFILE, &limit);
}

static void test_wav_write_failure(void) {
    if (access("/dev/full", W_OK) != 0) {
        fprintf(stderr, "SKIP wav write failure: no /dev/full\n");
        return;
    }
    if (symlink("/dev/full", path) != 0) {
        fail("wav write", "symlink");
        return;
    }
    PipeWireCapture *capture = open_script(
        "write(pcm(8192));await sleep(10000);");
    int code = 0;
    if (!capture || capture_wait(capture, 1, &code) != 0) fail("wav write", "unfinished");
    if (capture && capture_poll(capture, &code) != 0) fail("wav write", "poll");
    const char *error = capture ? capture_error(capture) : NULL;
    if (!error || (!strstr(error, "WAV") && !strstr(error, "capture failed"))) fail("wav write", error ? error : "null");
    capture_free(capture);
    unlink(path);
}

int test_capture(void) {
    failures = 0;
    if (getenv("ONLY_DELAY")) {
        setup(); test_delayed_samples(); teardown();
        setup(); test_odd_byte_boundary(); teardown();
        setup(); test_stall(); teardown();
        return failures;
    }
    setup(); test_preferred_target(); teardown();
    setup(); test_chunker(); teardown();
    setup(); test_pause_emits_before_end(); teardown();
    setup(); test_clipping(); teardown();
    setup(); test_ready_finalizes_wav(); teardown();
    setup(); test_no_samples(); teardown();
    setup(); test_producer_timeout_reaped(); teardown();
    setup(); test_delayed_samples(); teardown();
    setup(); test_odd_byte_boundary(); teardown();
    setup(); test_abnormal_exit(); teardown();
    setup(); test_exit_one_after_stop(); teardown();
    setup(); test_exit_one_without_stop(); teardown();
    setup(); test_stderr_not_hidden(); teardown();
    setup(); test_sample_limit_exit_one(); teardown();
    setup(); test_stall(); teardown();
    setup(); test_disconnect(); teardown();
    setup(); test_close_escalates(); teardown();
    setup(); test_constructor_failure_unlinks(); teardown();
    setup(); test_reader_start_failure(); teardown();
    setup(); test_cancel_before_ready(); teardown();
    setup(); test_wav_write_failure(); teardown();
    setup(); test_spawn_stdout_cloexec(); teardown();
    setup(); test_high_fd_read(); teardown();
    return failures;
}

#ifdef TEST_CAPTURE_MAIN
int test_devices(void);

int main(void) {
    int failed = test_capture() + test_devices();
    if (failed) fprintf(stderr, "%d capture/device tests failed\n", failed);
    return failed ? 1 : 0;
}
#endif
