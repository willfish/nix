#ifndef PI_VOICE_CAPTURE_H
#define PI_VOICE_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* 16 kHz mono s16 capture. SAMPLE_LIMIT is mutable so tests can patch it. */
extern int capture_sample_limit;

#define CAPTURE_RATE 16000
#define CAPTURE_FRAME 320
#define CAPTURE_VOICED_RMS 200
#define CAPTURE_SILENCE_FRAMES 30
#define CAPTURE_MIN_VOICED_FRAMES 8

typedef struct SpeechChunker SpeechChunker;
typedef struct PipeWireCapture PipeWireCapture;

typedef struct PcmChunk {
    int16_t *samples;
    size_t count;
} PcmChunk;

typedef struct CaptureProc {
    pid_t pid;
    int stdout_fd;
} CaptureProc;

/* Return 0 after filling out. stderr_fd is the capture diagnostic file. */
typedef int (*CaptureSpawnFn)(char *const *argv, int stderr_fd, CaptureProc *out, void *user);
typedef int (*CaptureReaderStartFn)(void *user);

extern CaptureSpawnFn capture_spawn_hook;
extern void *capture_spawn_hook_user;
extern CaptureReaderStartFn capture_reader_start_hook;
extern void *capture_reader_start_hook_user;
extern char capture_open_error[256];

void capture_test_reset(void);
int capture_spawn_default(char *const *argv, int stderr_fd, CaptureProc *out);

SpeechChunker *speech_chunker_new(int frame, int silence_frames, int force_frames, int lookback_frames);
void speech_chunker_free(SpeechChunker *chunker);
int speech_chunker_push(SpeechChunker *chunker, const int16_t *samples, size_t count, PcmChunk **out, size_t *out_count);
int speech_chunker_flush(SpeechChunker *chunker, PcmChunk *out);

/* command NULL builds pw-record. target is the stable node.name, or NULL. */
PipeWireCapture *capture_open(const char *path, char *const *command, const char *target);
void capture_close(PipeWireCapture *capture);
void capture_free(PipeWireCapture *capture);

int capture_wait_ready(PipeWireCapture *capture, double timeout);
int capture_wait(PipeWireCapture *capture, double timeout, int *code);
int capture_poll(PipeWireCapture *capture, int *code);
void capture_send_signal(PipeWireCapture *capture, int sig);
int capture_drain_chunks(PipeWireCapture *capture, PcmChunk **out, size_t *count);
int capture_wait_progress(PipeWireCapture *capture, double timeout);

const char *capture_error(PipeWireCapture *capture);
double capture_level(PipeWireCapture *capture);
int capture_clipping(PipeWireCapture *capture);
int capture_has_started(PipeWireCapture *capture);
double capture_started_at(PipeWireCapture *capture);
void pcm_chunks_free(PcmChunk *chunks, size_t count);

#endif
