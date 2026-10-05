#ifndef PI_VOICE_AUDIO_H
#define PI_VOICE_AUDIO_H

#include <stdatomic.h>
#include <stddef.h>

typedef struct audio audio;
typedef struct audio_player audio_player;
typedef struct audio_lease audio_lease;

typedef struct audio_pair {
    const char *key;
    const char *value;
} audio_pair;

typedef struct audio_voice {
    const char *id;
    const char *label;
    const audio_pair *options;
    size_t option_count;
} audio_voice;

typedef struct audio_config {
    const char *stt_url;
    int local_deepgram_api; /* local endpoints use the Deepgram REST subset */
    const char *stt_backend; /* explicit startup choice overrides saved preference */
    const char *speech_backend;
    const char *stt_health_url;
    const char *tts_url;
    const char *tts_health_url;
    int tts_enabled; /* nonzero enables local characters; zero is cloud-only */
    double readiness_timeout; /* seconds; 0 selects the 60 second default */
    const char *stt_prompt;
    const char *stt_language;
    const char *tts_model;
    const char *preferred_microphone;
    const char *voice_preferences_path;
    const char *stt_preferences_path;
    const char *playback_mode; /* NULL or "buffered"; "streaming" is the other mode */
    const audio_voice *voices;
    size_t voice_count;
    const audio_pair *long_voice;
    size_t long_voice_count;
} audio_config;

typedef struct audio_http_request {
    const char *method;
    const char *url;
    const char *content_type;
    const char *authorization;
    const unsigned char *body;
    size_t body_len;
    int timeout_ms;
    size_t maximum; /* 0 selects the transport default; a larger body is over the cap */
} audio_http_request;

typedef struct audio_http_response {
    int status;
    int transport_error; /* 1: connection or timeout, mapped by the caller */
    char verbatim_error[160]; /* propagated unchanged when set; never include secrets */
    unsigned char *body; /* malloc'd by the hook; freed by audio */
    size_t body_len;
} audio_http_response;

typedef int (*audio_http_fn)(const audio_http_request *request, audio_http_response *response, void *user);

typedef struct audio_spawn {
    const char *const *argv;
    int argc;
    int pass_fd; /* -1 when none; buffered playback passes the WAV fd */
    int pipe_stdin; /* 1 when streaming raw PCM */
} audio_spawn;

struct audio_player {
    int (*write)(audio_player *player, const void *data, size_t len);
    void (*close_stdin)(audio_player *player);
    /* timeout_ms < 0 waits until exit. Returns the exit status, or -1 on timeout. */
    int (*wait)(audio_player *player, int timeout_ms);
    /* -1 while running, otherwise the exit status. */
    int (*poll)(audio_player *player);
    void (*terminate)(audio_player *player);
    void (*kill)(audio_player *player);
    void (*destroy)(audio_player *player);
    int stdin_closed;
    void *user;
};

typedef audio_player *(*audio_popen_fn)(const audio_spawn *spawn, void *user);
/* 0 success, 1 timeout, other values are exit statuses. Must not abort the caller. */
typedef int (*audio_run_fn)(const char *const *argv, int argc, int timeout_sec, void *user);

struct audio_lease {
    int (*wait_ms)(audio_lease *lease, int timeout_ms, char *err, size_t err_cap);
    int (*ready_done)(audio_lease *lease);
    void (*release)(audio_lease *lease);
    void *user;
};

typedef audio_lease *(*audio_acquire_fn)(const char *engine, void *user);
typedef void (*audio_audible_fn)(void *user);
typedef void (*audio_mic_status_fn)(void *user, char *name, size_t name_cap, char *target, size_t target_cap);
typedef int (*audio_mic_resolve_fn)(void *user, const char *preferred, char *target, size_t target_cap, char *name, size_t name_cap);

/* Cached microphone metadata. Empty target means the PipeWire default, not failure.
 * muted_known is 0 when mute was not reported (JSON null). has_error is 0 when error is null.
 */
typedef struct audio_mic_details {
    char name[128];
    char target[128];
    int has_target;
    int muted;
    int muted_known;
    char preferred[512];
    int has_preferred;
    int missing;
    char error[160];
    int has_error;
} audio_mic_details;

typedef void (*audio_mic_details_fn)(void *user, audio_mic_details *out);
typedef void *(*audio_capture_fn)(const char *path, const char *target, void *user);
typedef void (*audio_drained_fn)(const char *text, void *user);

typedef struct audio_report {
    char selected_voice[64];
    char speech_backend[32];
    char selected_stt[32];
    char voice_ids[48][32];
    char voice_labels[48][64];
    size_t voice_count;
    char speech_backend_ids[4][32];
    char speech_backend_labels[4][64];
    size_t speech_backend_count;
    char stt_ids[4][32];
    char stt_labels[4][64];
    size_t stt_count;
    int has_stt;
    int has_tts;
    char stt_state[32];
    char tts_state[32];
    int has_stt_error;
    int has_tts_error;
    char stt_error[512];
    char tts_error[512];
    char microphone_name[128];
    char microphone_target[128];
    int microphone_has_target;
    int microphone_muted;
    int microphone_muted_known;
    char microphone_preferred[512];
    int microphone_has_preferred;
    int microphone_missing;
    char microphone_error[160];
    int microphone_has_error;
} audio_report;

void audio_config_init(audio_config *config);

audio *audio_new(const char *runtime_dir, const audio_config *config);
void audio_free(audio *audio);

void audio_set_http(audio *audio, audio_http_fn fn, void *user);
void audio_set_popen(audio *audio, audio_popen_fn fn, void *user);
void audio_set_run(audio *audio, audio_run_fn fn, void *user);
void audio_set_engines(audio *audio, audio_acquire_fn fn, void *user);
void audio_set_audible(audio *audio, audio_audible_fn fn, void *user);
void audio_set_microphone(audio *audio, audio_mic_status_fn status, audio_mic_resolve_fn resolve, void *user);
void audio_set_microphone_details(audio *audio, audio_mic_details_fn fn, void *user);
const char *audio_preferred_microphone(const audio *audio);
void audio_set_capture(audio *audio, audio_capture_fn fn, void *user);
void audio_set_playback_mode(audio *audio, const char *mode);

int audio_set_speech_backend(audio *audio, const char *backend, char *err, size_t err_cap);
int audio_set_voice(audio *audio, const char *character, char *err, size_t err_cap);
int audio_set_stt_backend(audio *audio, const char *backend, char *err, size_t err_cap);
int audio_status(audio *audio, audio_report *status);
/* Owned JSON object matching Python microphone status, including null target/muted/error.
 * Caller frees *json. 0 on success.
 */
int audio_microphone_json(audio *audio, char **json);
int audio_playing(audio *audio);

/* 1 ready, 0 cancelled, -1 error. */
int audio_wait_ready(audio *audio, const char *engine, atomic_int *cancelled, char *err, size_t err_cap);
/* 0 success or quiet cancel (out empty on cancel), -1 error. Never deletes path. */
int audio_transcribe(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user,
    char *out, size_t out_cap, char *err, size_t err_cap);
/* Same contract as audio_transcribe. destroy_ctx runs exactly once for drain_user:
 * before return on the waiting path, or from the HTTP worker after an abandoned
 * call returns. NULL destroy_ctx keeps the caller responsible for drain_user.
 */
int audio_transcribe_owned(audio *audio, const char *path, atomic_int *cancelled,
    audio_drained_fn on_drained, void *drain_user, void (*destroy_ctx)(void *ctx),
    char *out, size_t out_cap, char *err, size_t err_cap);
/* Blocks until detached audio workers have finished. */
void audio_drain(audio *audio);
/* 0 success or quiet cancel, -1 error. */
int audio_speak(audio *audio, const char *text, atomic_int *cancelled, char *err, size_t err_cap);
void audio_stop(audio *audio);
/* 0 played, -1 reportable failure. Never aborts the caller. */
int audio_cue(audio *audio, int frequency, char *err, size_t err_cap);
void *audio_start_capture(audio *audio, const char *path, char *err, size_t err_cap);

/* Test seam for a cancellable engine operation. 0 success, 1 abandoned, -1 error. */
int audio_call(audio *audio, const char *engine, atomic_int *cancelled,
    int (*op)(void *user, char *err, size_t err_cap), void *user,
    char *err, size_t err_cap);

int audio_speech_chunks(const char *text, char ***out, size_t *count);

int test_audio(void);

#endif
