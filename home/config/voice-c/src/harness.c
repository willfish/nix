#define _POSIX_C_SOURCE 200809L
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct voice_runtime {
    char *path;
    voice_config *config;
    audio *audio;
    engine_manager *engines;
    voice_audio_binding *binding;
    voice_terminal *terminal;
    int no_services;
    int closed;
};

typedef struct silent_player {
    audio_player api;
} silent_player;

static int silent_write(audio_player *player, const void *data, size_t len) {
    (void)player;
    (void)data;
    (void)len;
    return 0;
}
static void silent_close(audio_player *player) { player->stdin_closed = 1; }
static int silent_wait(audio_player *player, int timeout_ms) {
    (void)player;
    (void)timeout_ms;
    return 0;
}
static int silent_poll(audio_player *player) {
    (void)player;
    return 0;
}
static void silent_destroy(audio_player *player) { free(player); }

static audio_player *silent_popen(const audio_spawn *spawn, void *user) {
    (void)spawn;
    (void)user;
    silent_player *player = calloc(1, sizeof *player);
    if (!player) return NULL;
    player->api.write = silent_write;
    player->api.close_stdin = silent_close;
    player->api.wait = silent_wait;
    player->api.poll = silent_poll;
    player->api.terminate = silent_close;
    player->api.kill = silent_close;
    player->api.destroy = silent_destroy;
    return &player->api;
}

static int silent_run(const char *const *argv, int argc, int timeout_sec, void *user) {
    (void)argv;
    (void)argc;
    (void)timeout_sec;
    (void)user;
    return 0;
}

static int fixture_runner(const char *engine, const char *command, double timeout, void *ctx, char *err, size_t err_cap) {
    (void)engine;
    (void)command;
    (void)timeout;
    (void)ctx;
    (void)err;
    (void)err_cap;
    return 0;
}

static int fixture_ready(const char *engine, double timeout, void *ctx, char *err, size_t err_cap) {
    (void)engine;
    (void)timeout;
    (void)ctx;
    (void)err;
    (void)err_cap;
    return 1;
}

static int audio_ready(const char *engine, double timeout, void *ctx, char *err, size_t err_cap) {
    (void)timeout;
    atomic_int cancelled = 0;
    return audio_wait_ready(ctx, engine, &cancelled, err, err_cap);
}

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "");
}

voice_runtime *voice_runtime_open(
    const char *runtime_path, const yyjson_doc *config, const voice_runtime_spec *spec, char *err, size_t err_cap
) {
    char resolved[4096];
    if (!runtime_path || !runtime_path[0]) {
        if (voice_runtime_dir_resolve(resolved, sizeof resolved, err, err_cap) != 0) return NULL;
        runtime_path = resolved;
    }
    voice_runtime *runtime = calloc(1, sizeof *runtime);
    if (!runtime) return NULL;
    runtime->path = strdup(runtime_path);
    runtime->config = voice_config_parse(config, err, err_cap);
    runtime->no_services = spec && spec->no_services;
    if (!runtime->path || !runtime->config) {
        voice_runtime_close(runtime);
        set_err(err, err_cap, "could not open the voice runtime");
        return NULL;
    }
    runtime->audio = audio_new(runtime->path, voice_config_audio(runtime->config));
    if (!runtime->audio) {
        voice_runtime_close(runtime);
        set_err(err, err_cap, "could not create audio");
        return NULL;
    }
    if (spec && spec->silent) {
        audio_set_popen(runtime->audio, silent_popen, NULL);
        audio_set_run(runtime->audio, silent_run, NULL);
    }
    engine_config engines;
    memset(&engines, 0, sizeof engines);
    if (runtime->no_services) {
        engines.runner = fixture_runner;
        engines.readiness = fixture_ready;
    } else {
        engines.runner = voice_systemctl_runner;
        engines.readiness = audio_ready;
        engines.readiness_ctx = runtime->audio;
    }
    runtime->engines = engine_manager_create(&engines);
    if (!runtime->engines) {
        voice_runtime_close(runtime);
        set_err(err, err_cap, "could not create the engine manager");
        return NULL;
    }
    runtime->binding = voice_audio_bind(runtime->audio, runtime->engines, runtime->no_services, err, err_cap);
    runtime->terminal = voice_terminal_new();
    if (spec && spec->herdr_argv0) voice_terminal_set_herdr_argv0(runtime->terminal, spec->herdr_argv0);
    if (!runtime->binding || !runtime->terminal) {
        voice_runtime_close(runtime);
        set_err(err, err_cap, "could not bind the voice runtime");
        return NULL;
    }
    return runtime;
}

void voice_runtime_close(voice_runtime *runtime) {
    if (!runtime || runtime->closed) return;
    runtime->closed = 1;
    if (runtime->audio) {
        audio_stop(runtime->audio);
        audio_drain(runtime->audio);
    }
    voice_audio_unbind(runtime->binding);
    runtime->binding = NULL;
    if (runtime->engines) engine_manager_close(runtime->engines);
    if (runtime->audio) audio_free(runtime->audio);
    runtime->audio = NULL;
    if (runtime->engines) engine_manager_free(runtime->engines);
    runtime->engines = NULL;
    voice_terminal_free(runtime->terminal);
    runtime->terminal = NULL;
    voice_config_free(runtime->config);
    runtime->config = NULL;
    free(runtime->path);
    runtime->path = NULL;
    free(runtime);
}

audio *voice_runtime_audio(voice_runtime *runtime) { return runtime ? runtime->audio : NULL; }
engine_manager *voice_runtime_engines(voice_runtime *runtime) { return runtime ? runtime->engines : NULL; }
voice_terminal *voice_runtime_terminal(voice_runtime *runtime) { return runtime ? runtime->terminal : NULL; }
const char *voice_runtime_path(const voice_runtime *runtime) { return runtime ? runtime->path : NULL; }
int voice_runtime_auto_speak(const voice_runtime *runtime) { return voice_config_auto_speak(runtime ? runtime->config : NULL); }
