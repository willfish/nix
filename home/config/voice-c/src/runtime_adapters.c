#define _POSIX_C_SOURCE 200809L
#include "runtime_adapters.h"
#include "attachments.h"
#include "capture.h"
#include "devices.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { VOICE_PATH_MAX = 4096, VOICE_TEXT_MAX = 4096, VOICE_SHORT_MAX = 64 };

struct voice_target {
    voice_target_fields view;
    char socket[VOICE_PATH_MAX];
    char pane[VOICE_TEXT_MAX];
    char session[VOICE_TEXT_MAX];
    char token[VOICE_TEXT_MAX];
    char adapter_socket[VOICE_PATH_MAX];
    char bridge_id[VOICE_TEXT_MAX];
    char harness[VOICE_TEXT_MAX];
    char start[VOICE_SHORT_MAX];
};

struct voice_pi {
    int unused;
};

struct voice_herdr {
    char argv0[64];
    voice_process_fn process;
    void *process_user;
};

struct voice_terminal {
    voice_pi *pi;
    voice_herdr *herdr;
};

struct voice_config {
    audio_config audio;
    int auto_speak;
    char *stt_url;
    char *stt_health_url;
    char *tts_url;
    char *tts_health_url;
    char *stt_prompt;
    char *stt_language;
    char *tts_model;
    char *preferred_microphone;
    char *voice_preferences_path;
    char *stt_preferences_path;
    char *playback_mode;
    audio_voice *voices;
    audio_pair *long_voice;
    char **strings;
    size_t nstrings;
};

struct voice_audio_binding {
    audio *audio;
    engine_manager *engines;
    MicrophoneMonitor *mic;
    int no_services;
};

typedef struct bound_lease {
    audio_lease api;
    engine_lease *lease;
} bound_lease;

static void set_err(char *err, size_t cap, const char *msg) {
    if (!err || !cap) return;
    snprintf(err, cap, "%s", msg ? msg : "");
}

static int copy_bounded(char *dst, size_t cap, const char *src) {
    if (!src) {
        if (cap) dst[0] = '\0';
        return 0;
    }
    size_t n = strlen(src);
    if (n >= cap) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

static char *own_string(voice_config *config, const char *text) {
    if (!text) return NULL;
    char *copy = strdup(text);
    if (!copy) return NULL;
    char **next = realloc(config->strings, (config->nstrings + 1) * sizeof *next);
    if (!next) {
        free(copy);
        return NULL;
    }
    config->strings = next;
    config->strings[config->nstrings++] = copy;
    return copy;
}

static void bind_view(voice_target *target) {
    target->view.socket = target->socket[0] ? target->socket : NULL;
    target->view.pane = target->pane[0] ? target->pane : NULL;
    target->view.session = target->session[0] ? target->session : NULL;
    target->view.token = target->token[0] ? target->token : NULL;
    target->view.adapter_socket = target->adapter_socket[0] ? target->adapter_socket : NULL;
    target->view.bridge_id = target->bridge_id[0] ? target->bridge_id : NULL;
    target->view.harness = target->harness[0] ? target->harness : NULL;
    target->view.start = target->start[0] ? target->start : NULL;
}

voice_target *voice_target_new(const voice_target_fields *fields) {
    if (!fields) return NULL;
    voice_target *target = calloc(1, sizeof *target);
    if (!target) return NULL;
    if (copy_bounded(target->socket, sizeof target->socket, fields->socket) != 0 ||
        copy_bounded(target->pane, sizeof target->pane, fields->pane) != 0 ||
        copy_bounded(target->session, sizeof target->session, fields->session) != 0 ||
        copy_bounded(target->token, sizeof target->token, fields->token) != 0 ||
        copy_bounded(target->adapter_socket, sizeof target->adapter_socket, fields->adapter_socket) != 0 ||
        copy_bounded(target->bridge_id, sizeof target->bridge_id, fields->bridge_id) != 0 ||
        copy_bounded(target->harness, sizeof target->harness, fields->harness) != 0 ||
        copy_bounded(target->start, sizeof target->start, fields->start) != 0) {
        free(target);
        return NULL;
    }
    target->view.pid = fields->pid;
    target->view.managed = fields->managed ? 1 : 0;
    target->view.has_activation = fields->has_activation;
    target->view.activation = fields->activation;
    target->view.has_adapter_identity = fields->has_adapter_identity;
    target->view.adapter_device = fields->adapter_device;
    target->view.adapter_inode = fields->adapter_inode;
    target->view.has_socket_identity = fields->has_socket_identity;
    target->view.socket_device = fields->socket_device;
    target->view.socket_inode = fields->socket_inode;
    bind_view(target);
    return target;
}

static const char *json_str(yyjson_val *obj, const char *key) {
    yyjson_val *val = yyjson_obj_get(obj, key);
    return val && yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

static yyjson_val *imut_root(const yyjson_doc *doc) {
    return doc ? yyjson_doc_get_root((yyjson_doc *)doc) : NULL;
}

voice_target *voice_target_from_doc(const yyjson_doc *doc, char *err, size_t err_cap) {
    yyjson_val *root = imut_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        set_err(err, err_cap, "voice target must be a JSON object");
        return NULL;
    }
    voice_target_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.socket = json_str(root, "socket");
    fields.pane = json_str(root, "pane");
    fields.session = json_str(root, "session");
    fields.token = json_str(root, "token");
    fields.adapter_socket = json_str(root, "adapter_socket");
    fields.bridge_id = json_str(root, "bridge_id");
    fields.harness = json_str(root, "harness");
    fields.start = json_str(root, "start");
    yyjson_val *pid = yyjson_obj_get(root, "pid");
    if (pid && !yyjson_is_null(pid)) {
        if (!yyjson_is_int(pid)) {
            set_err(err, err_cap, "Pi process is unavailable");
            return NULL;
        }
        int64_t raw = yyjson_get_sint(pid);
        if (raw <= 0 || raw > INT_MAX) {
            set_err(err, err_cap, "Pi process is unavailable");
            return NULL;
        }
        fields.pid = (int)raw;
    }
    yyjson_val *managed = yyjson_obj_get(root, "managed");
    fields.managed = managed && yyjson_is_bool(managed) && yyjson_get_bool(managed);
    yyjson_val *activation = yyjson_obj_get(root, "activation");
    if (activation && yyjson_is_int(activation)) {
        fields.has_activation = 1;
        fields.activation = yyjson_get_sint(activation);
    }
    yyjson_val *adapter_device = yyjson_obj_get(root, "adapter_device");
    yyjson_val *adapter_inode = yyjson_obj_get(root, "adapter_inode");
    if (adapter_device && adapter_inode && yyjson_is_uint(adapter_device) && yyjson_is_uint(adapter_inode)) {
        fields.has_adapter_identity = 1;
        fields.adapter_device = yyjson_get_uint(adapter_device);
        fields.adapter_inode = yyjson_get_uint(adapter_inode);
    }
    yyjson_val *socket_device = yyjson_obj_get(root, "socket_device");
    yyjson_val *socket_inode = yyjson_obj_get(root, "socket_inode");
    if (socket_device && socket_inode && yyjson_is_uint(socket_device) && yyjson_is_uint(socket_inode)) {
        fields.has_socket_identity = 1;
        fields.socket_device = yyjson_get_uint(socket_device);
        fields.socket_inode = yyjson_get_uint(socket_inode);
    }
    voice_target *target = voice_target_new(&fields);
    if (!target) set_err(err, err_cap, "invalid voice target");
    return target;
}

void voice_target_free(voice_target *target) { free(target); }

const voice_target_fields *voice_target_get_fields(const voice_target *target) {
    return target ? &target->view : NULL;
}

int voice_incarnation_observe(const voice_target *target, voice_incarnation *out, char *err, size_t err_cap) {
    if (out) memset(out, 0, sizeof *out);
    if (!target || !out) return -1;
    const voice_target_fields *fields = &target->view;
    if (fields->pid > 0) {
        if (process_start_ticks(fields->pid, out->start, sizeof out->start) != 0) {
            set_err(err, err_cap, "Selected voice process has exited");
            return -1;
        }
        out->present = 1;
    }
    if (fields->managed && fields->adapter_socket && fields->bridge_id) {
        uint64_t dev = 0, ino = 0;
        const char *slash = strrchr(fields->adapter_socket, '/');
        char parent[VOICE_PATH_MAX];
        if (!slash || slash == fields->adapter_socket) {
            set_err(err, err_cap, "Selected Pi bridge is unavailable");
            return -1;
        }
        size_t n = (size_t)(slash - fields->adapter_socket);
        if (n >= sizeof parent) return -1;
        memcpy(parent, fields->adapter_socket, n);
        parent[n] = '\0';
        if (validate_endpoint(parent, fields->adapter_socket, fields->pid, fields->bridge_id, &dev, &ino) != 0) {
            set_err(err, err_cap, "Selected Pi bridge is unavailable");
            return -1;
        }
        out->has_adapter = 1;
        out->adapter_device = dev;
        out->adapter_inode = ino;
    }
    if (fields->socket) {
        char canonical[VOICE_PATH_MAX];
        uint64_t dev = 0, ino = 0;
        if (socket_instance(fields->socket, canonical, sizeof canonical, &dev, &ino) != 0) {
            set_err(err, err_cap, "Selected Herdr pane is unavailable");
            return -1;
        }
        out->has_socket = 1;
        out->socket_device = dev;
        out->socket_inode = ino;
    }
    return 0;
}

int voice_incarnation_matches(const voice_incarnation *expected, const voice_incarnation *observed) {
    if (!expected || !observed) return 0;
    if (expected->present != observed->present) return 0;
    if (expected->present && strcmp(expected->start, observed->start) != 0) return 0;
    if (expected->has_adapter != observed->has_adapter) return 0;
    if (expected->has_adapter &&
        (expected->adapter_device != observed->adapter_device || expected->adapter_inode != observed->adapter_inode))
        return 0;
    if (expected->has_socket != observed->has_socket) return 0;
    if (expected->has_socket &&
        (expected->socket_device != observed->socket_device || expected->socket_inode != observed->socket_inode))
        return 0;
    return 1;
}

static int pi_endpoint_ok(const voice_target *target, char *err, size_t err_cap) {
    const voice_target_fields *fields = &target->view;
    if (!fields->managed) return 0;
    char start[VOICE_SHORT_MAX];
    if (!fields->start || !fields->start[0] || fields->pid <= 0 ||
        process_start_ticks(fields->pid, start, sizeof start) != 0 || strcmp(start, fields->start) != 0) {
        set_err(err, err_cap, "Selected Pi bridge is unavailable");
        return -1;
    }
    if (!fields->adapter_socket || !fields->bridge_id || !fields->has_adapter_identity) {
        set_err(err, err_cap, "Selected Pi bridge is unavailable");
        return -1;
    }
    const char *slash = strrchr(fields->adapter_socket, '/');
    char parent[VOICE_PATH_MAX];
    if (!slash || slash == fields->adapter_socket) {
        set_err(err, err_cap, "Selected Pi bridge is unavailable");
        return -1;
    }
    size_t n = (size_t)(slash - fields->adapter_socket);
    if (n >= sizeof parent) {
        set_err(err, err_cap, "Selected Pi bridge is unavailable");
        return -1;
    }
    memcpy(parent, fields->adapter_socket, n);
    parent[n] = '\0';
    uint64_t dev = 0, ino = 0;
    if (validate_endpoint(parent, fields->adapter_socket, fields->pid, fields->bridge_id, &dev, &ino) != 0 ||
        dev != fields->adapter_device || ino != fields->adapter_inode) {
        set_err(err, err_cap, "Selected Pi bridge is unavailable");
        return -1;
    }
    return 0;
}

int voice_runtime_dir_resolve(char *out, size_t cap, char *err, size_t err_cap) {
    if (!out || cap < 16) return -1;
    const char *base = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (!base || !base[0] || base[0] != '/') {
        snprintf(fallback, sizeof fallback, "/run/user/%u", (unsigned)getuid());
        base = fallback;
    }
    if (snprintf(out, cap, "%s/pi-voice", base) >= (int)cap) {
        set_err(err, err_cap, "voice runtime path is too long");
        return -1;
    }
    struct stat st;
    if (lstat(out, &st) != 0) {
        if (mkdir(out, 0700) != 0 && errno != EEXIST) {
            set_err(err, err_cap, "could not create the voice runtime directory");
            return -1;
        }
    }
    if (lstat(out, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) {
        set_err(err, err_cap, "Pi runtime directory must be private and owned");
        return -1;
    }
    return 0;
}

static int run_hooked(const ipc_process_request *request, ipc_process_result *result, void *ctx, char *err, size_t err_cap) {
    voice_process_hook *hook = ctx;
    if (hook && hook->fn) return hook->fn(request, result, hook->user, err, err_cap);
    return ipc_process_run(request, result, err, err_cap);
}

static int systemctl_exec(const char *const *argv, int argc, double timeout, void *ctx, char *stdout_buf, size_t cap) {
    ipc_process_request request;
    memset(&request, 0, sizeof request);
    request.argv = argv;
    request.argc = argc;
    request.capture_stdout = stdout_buf != NULL;
    request.deadline_ms = timeout > 0 ? (int)(timeout * 1000.0) : 1000;
    if (request.deadline_ms < 1) request.deadline_ms = 1;
    if (request.deadline_ms > 120000) request.deadline_ms = 120000;
    request.stdout_max = cap ? cap : 256;
    ipc_process_result result;
    memset(&result, 0, sizeof result);
    char err[128];
    int rc = run_hooked(&request, &result, ctx, err, sizeof err);
    int failed = rc != IPC_OK || result.exit_code != 0;
    if (!failed && stdout_buf && cap) {
        size_t n = result.stdout_len;
        if (n >= cap) n = cap - 1;
        if (result.stdout_bytes && n) memcpy(stdout_buf, result.stdout_bytes, n);
        stdout_buf[n] = '\0';
    }
    ipc_process_result_free(&result);
    return failed ? -1 : 0;
}

int voice_systemctl_runner(
    const char *engine, const char *command, double timeout, void *ctx, char *err, size_t err_cap
) {
    if (!command || !command[0] ||
        (strcmp(command, "start") != 0 && strcmp(command, "stop") != 0 && strcmp(command, "restart") != 0)) {
        set_err(err, err_cap, "invalid systemctl command");
        return ENGINE_INVALID;
    }
    char unit[64];
    if (!engine || (strcmp(engine, "stt") != 0 && strcmp(engine, "tts") != 0)) return ENGINE_INVALID;
    snprintf(unit, sizeof unit, "pi-voice-%s.service", engine);
    const char *argv[] = {"systemctl", "--user", command, unit};
    if (systemctl_exec(argv, 4, timeout, ctx, NULL, 0) != 0) {
        set_err(err, err_cap, "systemctl command failed");
        return ENGINE_ERR;
    }
    return ENGINE_OK;
}

int voice_systemctl_activity(
    const char *engine, double timeout, void *ctx, int *active, char *err, size_t err_cap
) {
    if (active) *active = 0;
    if (!active) return ENGINE_ERR;
    char unit[64];
    if (!engine || (strcmp(engine, "stt") != 0 && strcmp(engine, "tts") != 0)) return ENGINE_INVALID;
    snprintf(unit, sizeof unit, "pi-voice-%s.service", engine);
    const char *argv[] = {"systemctl", "--user", "show", "--property=ActiveState", "--value", unit};
    char text[128];
    text[0] = '\0';
    if (systemctl_exec(argv, 6, timeout, ctx, text, sizeof text) != 0) {
        set_err(err, err_cap, "activity query failed");
        return ENGINE_ERR;
    }
    return engine_active_from_text(engine, text, active, err, err_cap);
}

static const char *opt_str(yyjson_val *obj, const char *key) {
    yyjson_val *val = yyjson_obj_get(obj, key);
    if (!val || yyjson_is_null(val)) return NULL;
    return yyjson_is_str(val) ? yyjson_get_str(val) : NULL;
}

voice_config *voice_config_parse(const yyjson_doc *doc, char *err, size_t err_cap) {
    yyjson_val *root = imut_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        set_err(err, err_cap, "voice config must be a JSON object");
        return NULL;
    }
    voice_config *config = calloc(1, sizeof *config);
    if (!config) return NULL;
    audio_config_init(&config->audio);
    config->auto_speak = 1;
    yyjson_val *speak = yyjson_obj_get(root, "auto_speak");
    if (speak && yyjson_is_bool(speak)) config->auto_speak = yyjson_get_bool(speak);
    yyjson_val *enabled = yyjson_obj_get(root, "tts_enabled");
    config->audio.tts_enabled = !enabled || !yyjson_is_bool(enabled) || yyjson_get_bool(enabled);
    yyjson_val *timeout = yyjson_obj_get(root, "readiness_timeout");
    if (timeout && yyjson_is_num(timeout)) config->audio.readiness_timeout = yyjson_get_num(timeout);
    yyjson_val *api = yyjson_obj_get(root, "local_deepgram_api");
    config->audio.local_deepgram_api = yyjson_is_true(api);
    config->audio.stt_backend = own_string(config, opt_str(root, "stt_backend"));
    config->audio.speech_backend = own_string(config, opt_str(root, "speech_backend"));
    if ((config->audio.stt_backend && strcmp(config->audio.stt_backend, "whisper") && strcmp(config->audio.stt_backend, "deepgram")) ||
        (config->audio.speech_backend && strcmp(config->audio.speech_backend, "local") && strcmp(config->audio.speech_backend, "deepgram"))) {
        voice_config_free(config);
        set_err(err, err_cap, "invalid configured voice backend");
        return NULL;
    }
    config->stt_url = own_string(config, opt_str(root, "stt_url"));
    config->stt_health_url = own_string(config, opt_str(root, "stt_health_url"));
    config->tts_url = own_string(config, opt_str(root, "tts_url"));
    config->tts_health_url = own_string(config, opt_str(root, "tts_health_url"));
    config->stt_prompt = own_string(config, opt_str(root, "stt_prompt"));
    config->stt_language = own_string(config, opt_str(root, "stt_language"));
    config->tts_model = own_string(config, opt_str(root, "tts_model"));
    config->preferred_microphone = own_string(config, opt_str(root, "preferred_microphone"));
    config->voice_preferences_path = own_string(config, opt_str(root, "voice_preferences_path"));
    config->stt_preferences_path = own_string(config, opt_str(root, "stt_preferences_path"));
    config->playback_mode = own_string(config, opt_str(root, "playback_mode"));
    config->audio.stt_url = config->stt_url;
    config->audio.stt_health_url = config->stt_health_url;
    config->audio.tts_url = config->tts_url;
    config->audio.tts_health_url = config->tts_health_url;
    config->audio.stt_prompt = config->stt_prompt;
    config->audio.stt_language = config->stt_language;
    config->audio.tts_model = config->tts_model;
    config->audio.preferred_microphone = config->preferred_microphone;
    config->audio.voice_preferences_path = config->voice_preferences_path;
    config->audio.stt_preferences_path = config->stt_preferences_path;
    config->audio.playback_mode = config->playback_mode;

    yyjson_val *voices = yyjson_obj_get(root, "tts_voices");
    if (voices && yyjson_is_obj(voices)) {
        size_t count = yyjson_obj_size(voices);
        if (count > 47) {
            voice_config_free(config);
            set_err(err, err_cap, "too many TTS voices");
            return NULL;
        }
        config->voices = calloc(count ? count : 1, sizeof *config->voices);
        if (!config->voices) {
            voice_config_free(config);
            return NULL;
        }
        size_t index = 0;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(voices, &iter);
        yyjson_val *key, *val;
        while ((key = yyjson_obj_iter_next(&iter))) {
            val = yyjson_obj_iter_get_val(key);
            if (!yyjson_is_obj(val)) continue;
            audio_voice *voice = &config->voices[index++];
            voice->id = own_string(config, yyjson_get_str(key));
            voice->label = own_string(config, opt_str(val, "label"));
            if (!voice->label) voice->label = voice->id;
            yyjson_val *options = yyjson_obj_get(val, "options");
            if (options && yyjson_is_obj(options)) {
                size_t n = yyjson_obj_size(options);
                if (n > 16) {
                    voice_config_free(config);
                    set_err(err, err_cap, "too many voice options");
                    return NULL;
                }
                typedef struct mutable_pair { char *key; char *value; } mutable_pair;
                mutable_pair *pairs = calloc(n ? n : 1, sizeof *pairs);
                voice->options = (const audio_pair *)pairs;
                voice->option_count = 0;
                yyjson_obj_iter opt;
                yyjson_obj_iter_init(options, &opt);
                yyjson_val *okey, *oval;
                while ((okey = yyjson_obj_iter_next(&opt))) {
                    oval = yyjson_obj_iter_get_val(okey);
                    if (!yyjson_is_str(oval)) continue;
                    pairs[voice->option_count].key = own_string(config, yyjson_get_str(okey));
                    pairs[voice->option_count].value = own_string(config, yyjson_get_str(oval));
                    voice->option_count++;
                }
            }
        }
        config->audio.voices = config->voices;
        config->audio.voice_count = index;
    }
    yyjson_val *long_voice = yyjson_obj_get(root, "tts_long_voice");
    if (long_voice && yyjson_is_obj(long_voice)) {
        size_t n = yyjson_obj_size(long_voice);
        if (n > 16) {
            voice_config_free(config);
            set_err(err, err_cap, "too many long-voice options");
            return NULL;
        }
        typedef struct mutable_pair { char *key; char *value; } mutable_pair;
        mutable_pair *pairs = calloc(n ? n : 1, sizeof *pairs);
        config->long_voice = (audio_pair *)pairs;
        size_t index = 0;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(long_voice, &iter);
        yyjson_val *key, *val;
        while ((key = yyjson_obj_iter_next(&iter))) {
            val = yyjson_obj_iter_get_val(key);
            if (!yyjson_is_str(val)) continue;
            pairs[index].key = own_string(config, yyjson_get_str(key));
            pairs[index].value = own_string(config, yyjson_get_str(val));
            index++;
        }
        config->audio.long_voice = config->long_voice;
        config->audio.long_voice_count = index;
    }
    return config;
}

void voice_config_free(voice_config *config) {
    if (!config) return;
    if (config->voices) {
        for (size_t i = 0; i < config->audio.voice_count; i++) free((void *)config->voices[i].options);
    }
    free(config->voices);
    free((void *)config->long_voice);
    for (size_t i = 0; i < config->nstrings; i++) free(config->strings[i]);
    free(config->strings);
    free(config);
}

const audio_config *voice_config_audio(const voice_config *config) {
    return config ? &config->audio : NULL;
}

int voice_config_auto_speak(const voice_config *config) {
    return config && config->auto_speak;
}

static int bound_wait(audio_lease *lease, int timeout_ms, char *err, size_t err_cap) {
    bound_lease *bound = (bound_lease *)lease;
    int status = engine_lease_wait(bound->lease, timeout_ms, err, err_cap);
    if (status == ENGINE_WAIT_OK) return 0;
    if (status == ENGINE_WAIT_UNREADY) return 1;
    return -1;
}

static int bound_done(audio_lease *lease) {
    bound_lease *bound = (bound_lease *)lease;
    return engine_lease_done(bound->lease);
}

static void bound_release(audio_lease *lease) {
    bound_lease *bound = (bound_lease *)lease;
    engine_lease_release(bound->lease);
    engine_lease_unref(bound->lease);
    free(bound);
}

static audio_lease *bind_acquire(const char *engine, void *user) {
    voice_audio_binding *binding = user;
    engine_lease *lease = NULL;
    if (engine_manager_acquire(binding->engines, engine, &lease) != ENGINE_OK || !lease) return NULL;
    bound_lease *bound = calloc(1, sizeof *bound);
    if (!bound) {
        engine_lease_release(lease);
        engine_lease_unref(lease);
        return NULL;
    }
    bound->lease = lease;
    bound->api.wait_ms = bound_wait;
    bound->api.ready_done = bound_done;
    bound->api.release = bound_release;
    bound->api.user = bound;
    return &bound->api;
}

static int fixture_mic(char *const *argv, double timeout, MicRunResult *out, void *user) {
    (void)argv;
    (void)timeout;
    (void)user;
    memset(out, 0, sizeof *out);
    out->ok = 1;
    out->stdout_text = strdup("[]");
    return out->stdout_text ? 0 : -1;
}

static void mic_status_cb(void *user, char *name, size_t name_cap, char *target, size_t target_cap) {
    voice_audio_binding *binding = user;
    MicStatus status;
    memset(&status, 0, sizeof status);
    if (name && name_cap) name[0] = '\0';
    if (target && target_cap) target[0] = '\0';
    if (mic_status(binding->mic, &status) != 0) return;
    if (name && name_cap) snprintf(name, name_cap, "%s", status.name ? status.name : "");
    if (target && target_cap) snprintf(target, target_cap, "%s", status.target ? status.target : "");
    mic_status_free(&status);
}

static int mic_resolve_cb(void *user, const char *preferred, char *target, size_t target_cap, char *name, size_t name_cap) {
    (void)preferred;
    voice_audio_binding *binding = user;
    MicStatus status;
    memset(&status, 0, sizeof status);
    if (mic_resolve(binding->mic, &status) != 0) return -1;
    /* An empty target is the PipeWire default. Preferred-missing is a warning, not a capture failure. */
    if (name && name_cap) snprintf(name, name_cap, "%s", status.name ? status.name : "System default");
    if (target && target_cap) snprintf(target, target_cap, "%s", status.target ? status.target : "");
    mic_status_free(&status);
    return 0;
}

static void copy_cap(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static void mic_details_cb(void *user, audio_mic_details *out) {
    voice_audio_binding *binding = user;
    MicStatus status;
    memset(&status, 0, sizeof status);
    memset(out, 0, sizeof *out);
    if (mic_status(binding->mic, &status) != 0) {
        copy_cap(out->name, sizeof out->name, "System default");
        copy_cap(out->error, sizeof out->error, "Microphone details unavailable");
        out->has_error = 1;
        return;
    }
    copy_cap(out->name, sizeof out->name, status.name ? status.name : "System default");
    if (status.target && status.target[0]) {
        copy_cap(out->target, sizeof out->target, status.target);
        out->has_target = 1;
    }
    out->muted = status.muted;
    out->muted_known = status.muted_known;
    if (status.preferred && status.preferred[0]) {
        copy_cap(out->preferred, sizeof out->preferred, status.preferred);
        out->has_preferred = 1;
    }
    out->missing = status.missing;
    if (status.error && status.error[0]) {
        copy_cap(out->error, sizeof out->error, status.error);
        out->has_error = 1;
    }
    mic_status_free(&status);
}

static void *capture_cb(const char *path, const char *target, void *user) {
    (void)user;
    return capture_open(path, NULL, target && target[0] ? target : NULL);
}

voice_audio_binding *voice_audio_bind_with_runner(
    audio *audio, engine_manager *engines, int no_services,
    MicRunner runner, void *runner_user, char *err, size_t err_cap
) {
    if (!audio || !engines) {
        set_err(err, err_cap, "audio binding is missing an engine manager");
        return NULL;
    }
    voice_audio_binding *binding = calloc(1, sizeof *binding);
    if (!binding) return NULL;
    binding->audio = audio;
    binding->engines = engines;
    binding->no_services = no_services;
    if (!runner) runner = no_services ? fixture_mic : mic_runner_pw_dump;
    binding->mic = mic_monitor_new(audio_preferred_microphone(audio), runner, runner_user);
    if (!binding->mic) {
        free(binding);
        set_err(err, err_cap, "could not watch the microphone");
        return NULL;
    }
    audio_set_engines(audio, bind_acquire, binding);
    audio_set_microphone(audio, mic_status_cb, mic_resolve_cb, binding);
    audio_set_microphone_details(audio, mic_details_cb, binding);
    audio_set_capture(audio, capture_cb, binding);
    return binding;
}

voice_audio_binding *voice_audio_bind(audio *audio, engine_manager *engines, int no_services, char *err, size_t err_cap) {
    return voice_audio_bind_with_runner(audio, engines, no_services, NULL, NULL, err, err_cap);
}

void voice_audio_unbind(voice_audio_binding *binding) {
    if (!binding) return;
    if (binding->audio) {
        audio_set_engines(binding->audio, NULL, NULL);
        audio_set_microphone(binding->audio, NULL, NULL, NULL);
        audio_set_microphone_details(binding->audio, NULL, NULL);
        audio_set_capture(binding->audio, NULL, NULL);
    }
    mic_monitor_free(binding->mic);
    free(binding);
}

voice_pi *voice_pi_new(void) { return calloc(1, sizeof(voice_pi)); }
void voice_pi_free(voice_pi *pi) { free(pi); }

static yyjson_doc *doc_from_val(yyjson_val *val) {
    if (!val) return NULL;
    yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
    if (!mut) return NULL;
    yyjson_mut_val *copied = yyjson_val_mut_copy(mut, val);
    if (!copied) {
        yyjson_mut_doc_free(mut);
        return NULL;
    }
    yyjson_mut_doc_set_root(mut, copied);
    yyjson_doc *doc = yyjson_mut_doc_imut_copy(mut, NULL);
    yyjson_mut_doc_free(mut);
    return doc;
}

static int cancelled_now(atomic_int *cancelled) {
    return cancelled && atomic_load(cancelled);
}

int voice_pi_request(
    voice_pi *pi, const voice_target *target, const char *command, const yyjson_doc *fields,
    yyjson_doc **result, char *err, size_t err_cap
) {
    (void)pi;
    if (result) *result = NULL;
    if (!target || !command || !command[0]) {
        set_err(err, err_cap, "invalid Pi request");
        return VOICE_REJECTED;
    }
    if (pi_endpoint_ok(target, err, err_cap) != 0) return VOICE_REJECTED;
    const voice_target_fields *view = &target->view;
    if (!view->token || !view->token[0] || !view->session || !view->session[0]) {
        set_err(err, err_cap, "Pi voice session is not bound yet");
        return VOICE_REJECTED;
    }
    if (!view->adapter_socket) {
        set_err(err, err_cap, "Selected Pi session is unavailable");
        return VOICE_REJECTED;
    }
    yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
    if (!mut) return VOICE_REJECTED;
    yyjson_mut_val *obj = yyjson_mut_obj(mut);
    yyjson_mut_doc_set_root(mut, obj);
    yyjson_mut_obj_add_strncpy(mut, obj, "token", view->token, strlen(view->token));
    yyjson_mut_obj_add_strncpy(mut, obj, "session", view->session, strlen(view->session));
    yyjson_mut_obj_add_strncpy(mut, obj, "command", command, strlen(command));
    if (view->managed) {
        if (view->bridge_id) yyjson_mut_obj_add_strncpy(mut, obj, "bridge_id", view->bridge_id, strlen(view->bridge_id));
        if (view->has_activation) yyjson_mut_obj_add_sint(mut, obj, "activation", view->activation);
        yyjson_mut_obj_add_int(mut, obj, "pid", view->pid);
        if (view->harness) yyjson_mut_obj_add_strncpy(mut, obj, "harness", view->harness, strlen(view->harness));
    }
    if (fields) {
        yyjson_val *extra = imut_root(fields);
        if (extra && yyjson_is_obj(extra)) {
            yyjson_obj_iter iter;
            yyjson_obj_iter_init(extra, &iter);
            yyjson_val *key, *val;
            while ((key = yyjson_obj_iter_next(&iter))) {
                val = yyjson_obj_iter_get_val(key);
                yyjson_mut_val *copied = yyjson_val_mut_copy(mut, val);
                if (copied) yyjson_mut_obj_add_val(mut, obj, yyjson_get_str(key), copied);
            }
        }
    }
    yyjson_doc *request = yyjson_mut_doc_imut_copy(mut, NULL);
    yyjson_mut_doc_free(mut);
    if (!request) return VOICE_REJECTED;
    ipc_unix_request call = {
        .socket_path = view->adapter_socket,
        .request = request,
        .deadline_ms = 2000,
        .max_response = 256 * 1024,
    };
    int sent = 0;
    yyjson_doc *response = NULL;
    int rc = ipc_unix_json(&call, &sent, &response, err, err_cap);
    yyjson_doc_free(request);
    int status_cmd = strcmp(command, "status") == 0;
    if (rc != IPC_OK) {
        yyjson_doc_free(response);
        if (!sent) {
            set_err(err, err_cap, "Selected Pi session is unavailable");
            return VOICE_REJECTED;
        }
        set_err(err, err_cap, "Could not confirm Pi delivery; check its prompt");
        return status_cmd ? VOICE_REJECTED : VOICE_UNCERTAIN;
    }
    if (pi_endpoint_ok(target, err, err_cap) != 0) {
        yyjson_doc_free(response);
        if (!status_cmd) {
            set_err(err, err_cap, "Pi bridge changed during delivery; check its prompt");
            return VOICE_UNCERTAIN;
        }
        return VOICE_REJECTED;
    }
    yyjson_val *root = yyjson_doc_get_root(response);
    yyjson_val *ok = yyjson_obj_get(root, "ok");
    if (!ok || !yyjson_is_bool(ok)) {
        yyjson_doc_free(response);
        set_err(err, err_cap, "Could not confirm Pi delivery; check its prompt");
        return status_cmd ? VOICE_REJECTED : VOICE_UNCERTAIN;
    }
    if (!yyjson_get_bool(ok)) {
        yyjson_val *uncertain = yyjson_obj_get(root, "uncertain");
        const char *message = json_str(root, "error");
        int is_uncertain = uncertain && yyjson_is_bool(uncertain) && yyjson_get_bool(uncertain);
        set_err(err, err_cap, message ? message : "Pi voice command failed");
        yyjson_doc_free(response);
        return is_uncertain ? VOICE_UNCERTAIN : VOICE_REJECTED;
    }
    yyjson_val *body = yyjson_obj_get(root, "result");
    if (!body || !yyjson_is_obj(body)) {
        yyjson_doc_free(response);
        set_err(err, err_cap, "Invalid Pi voice acknowledgement");
        return status_cmd ? VOICE_REJECTED : VOICE_UNCERTAIN;
    }
    if (result) *result = doc_from_val(body);
    yyjson_doc_free(response);
    if (result && !*result) return VOICE_REJECTED;
    return VOICE_OK;
}

static int event_same(const voice_target *target, yyjson_val *status) {
    const voice_target_fields *view = &target->view;
    EventView expected = {0}, observed = {0};
    expected.bridge_id = view->bridge_id;
    expected.has_activation = view->has_activation;
    expected.activation = view->activation;
    expected.has_pid = 1;
    expected.pid = view->pid;
    expected.session = view->session;
    expected.harness = view->harness;
    const char *bridge = json_str(status, "bridge_id");
    observed.bridge_id = bridge;
    yyjson_val *activation = yyjson_obj_get(status, "activation");
    if (activation && yyjson_is_int(activation)) {
        observed.has_activation = 1;
        observed.activation = yyjson_get_sint(activation);
    }
    yyjson_val *pid = yyjson_obj_get(status, "pid");
    if (pid && yyjson_is_int(pid)) {
        observed.has_pid = 1;
        observed.pid = yyjson_get_int(pid);
    }
    observed.session = json_str(status, "session");
    observed.harness = json_str(status, "harness");
    return event_matches(&expected, &observed, 1);
}

int voice_pi_validate_target(voice_pi *pi, const voice_target *target, yyjson_doc **status, char *err, size_t err_cap) {
    if (status) *status = NULL;
    yyjson_doc *body = NULL;
    int rc = voice_pi_request(pi, target, "status", NULL, &body, err, err_cap);
    if (rc != VOICE_OK) return rc;
    yyjson_val *root = yyjson_doc_get_root(body);
    const voice_target_fields *view = &target->view;
    yyjson_val *pid = yyjson_obj_get(root, "pid");
    const char *session = json_str(root, "session");
    int pid_ok = pid && yyjson_is_int(pid) && yyjson_get_int(pid) == view->pid;
    if (!session || strcmp(session, view->session ? view->session : "") != 0 || !pid_ok ||
        (view->managed && !event_same(target, root))) {
        yyjson_doc_free(body);
        set_err(err, err_cap, "Pi session changed; rebind voice");
        return VOICE_REJECTED;
    }
    if (status) *status = body;
    else yyjson_doc_free(body);
    return VOICE_OK;
}

int voice_pi_validate(voice_pi *pi, const voice_target *target, char *err, size_t err_cap) {
    yyjson_doc *status = NULL;
    int rc = voice_pi_validate_target(pi, target, &status, err, err_cap);
    if (rc != VOICE_OK) return rc;
    yyjson_val *root = yyjson_doc_get_root(status);
    yyjson_val *accepts = yyjson_obj_get(root, "accepts_input");
    if (!accepts) accepts = yyjson_obj_get(root, "ready");
    int ok = accepts && yyjson_is_bool(accepts) && yyjson_get_bool(accepts);
    yyjson_doc_free(status);
    if (!ok) {
        set_err(err, err_cap, "Pi is busy or waiting for an interaction");
        return VOICE_REJECTED;
    }
    return VOICE_OK;
}

int voice_pi_insert_guarded(
    voice_pi *pi, const voice_target *target, const char *text, atomic_int *cancelled, char *err, size_t err_cap
) {
    if (cancelled_now(cancelled)) {
        set_err(err, err_cap, "Voice delivery was cancelled");
        return VOICE_CANCELLED;
    }
    int rc = voice_pi_validate(pi, target, err, err_cap);
    if (rc != VOICE_OK) return rc;
    if (cancelled_now(cancelled)) {
        set_err(err, err_cap, "Voice delivery was cancelled");
        return VOICE_CANCELLED;
    }
    yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(mut);
    yyjson_mut_doc_set_root(mut, obj);
    yyjson_mut_obj_add_strncpy(mut, obj, "text", text ? text : "", text ? strlen(text) : 0);
    yyjson_doc *fields = yyjson_mut_doc_imut_copy(mut, NULL);
    yyjson_mut_doc_free(mut);
    rc = voice_pi_request(pi, target, "stage", fields, NULL, err, err_cap);
    yyjson_doc_free(fields);
    return rc;
}

int voice_pi_submit_guarded(
    voice_pi *pi, const voice_target *target, atomic_int *cancelled, int allow_edited, char *err, size_t err_cap
) {
    if (cancelled_now(cancelled)) {
        set_err(err, err_cap, "Voice delivery was cancelled");
        return VOICE_CANCELLED;
    }
    int rc = voice_pi_validate(pi, target, err, err_cap);
    if (rc != VOICE_OK) return rc;
    if (cancelled_now(cancelled)) {
        set_err(err, err_cap, "Voice delivery was cancelled");
        return VOICE_CANCELLED;
    }
    yyjson_doc *fields = NULL;
    if (allow_edited) {
        yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *obj = yyjson_mut_obj(mut);
        yyjson_mut_doc_set_root(mut, obj);
        yyjson_mut_obj_add_bool(mut, obj, "allow_edited", true);
        fields = yyjson_mut_doc_imut_copy(mut, NULL);
        yyjson_mut_doc_free(mut);
    }
    rc = voice_pi_request(pi, target, "submit", fields, NULL, err, err_cap);
    yyjson_doc_free(fields);
    return rc;
}

voice_herdr *voice_herdr_new(const char *argv0) {
    voice_herdr *herdr = calloc(1, sizeof *herdr);
    if (!herdr) return NULL;
    snprintf(herdr->argv0, sizeof herdr->argv0, "%s", argv0 && argv0[0] ? argv0 : "herdr");
    return herdr;
}

void voice_herdr_free(voice_herdr *herdr) { free(herdr); }

void voice_herdr_set_process(voice_herdr *herdr, voice_process_fn fn, void *user) {
    if (!herdr) return;
    herdr->process = fn;
    herdr->process_user = user;
}

static int herdr_start_ok(const voice_target *target, char *err, size_t err_cap) {
    const voice_target_fields *view = &target->view;
    char start[VOICE_SHORT_MAX];
    if (!view->start || !view->start[0] || view->pid <= 0 ||
        process_start_ticks(view->pid, start, sizeof start) != 0 || strcmp(start, view->start) != 0) {
        set_err(err, err_cap, "Selected agent has exited; start its voice launcher again");
        return -1;
    }
    return 0;
}

int voice_herdr_request(
    voice_herdr *herdr, const voice_target *target, const char *const *args, int argc, int mutation,
    yyjson_doc **result, char *err, size_t err_cap
) {
    if (result) *result = NULL;
    if (!herdr || !target || !args || argc < 1 || argc > 8 || !target->view.socket) {
        set_err(err, err_cap, "Could not reach the selected Herdr pane");
        return VOICE_REJECTED;
    }
    char start_before[VOICE_SHORT_MAX];
    start_before[0] = '\0';
    if (target->view.pid > 0) process_start_ticks(target->view.pid, start_before, sizeof start_before);
    const char *argv[10];
    argv[0] = herdr->argv0;
    for (int i = 0; i < argc; i++) argv[i + 1] = args[i];
    ipc_env_override env = {.key = "HERDR_SOCKET_PATH", .value = target->view.socket};
    ipc_process_request request;
    memset(&request, 0, sizeof request);
    request.argv = argv;
    request.argc = argc + 1;
    request.capture_stdout = 1;
    request.capture_stderr = 1;
    request.deadline_ms = target->view.managed ? 1000 : 8000;
    request.stdout_max = 1024 * 1024;
    request.stderr_max = 4096;
    request.env = &env;
    request.env_count = 1;
    ipc_process_result output;
    memset(&output, 0, sizeof output);
    voice_process_hook hook = {.fn = herdr->process, .user = herdr->process_user};
    int rc = run_hooked(&request, &output, herdr->process ? &hook : NULL, err, err_cap);
    int failed = rc != IPC_OK || output.exit_code != 0 || !output.stdout_bytes;
    if (failed) {
        ipc_process_result_free(&output);
        set_err(err, err_cap, "Could not reach the selected Herdr pane");
        return VOICE_REJECTED;
    }
    yyjson_doc *doc = yyjson_read((char *)output.stdout_bytes, output.stdout_len, 0);
    ipc_process_result_free(&output);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *body = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "result") : NULL;
    if (!body) {
        yyjson_doc_free(doc);
        set_err(err, err_cap, "Invalid response from the Herdr pane");
        return VOICE_REJECTED;
    }
    if (mutation && target->view.pid > 0) {
        char start_after[VOICE_SHORT_MAX];
        if (process_start_ticks(target->view.pid, start_after, sizeof start_after) != 0 ||
            strcmp(start_after, start_before) != 0) {
            yyjson_doc_free(doc);
            set_err(err, err_cap, "Could not confirm paste; check the selected Pi prompt and send it there if the dictation arrived");
            return VOICE_UNCERTAIN;
        }
    }
    if (result) *result = doc_from_val(body);
    yyjson_doc_free(doc);
    if (result && !*result) {
        set_err(err, err_cap, "Invalid response from the Herdr pane");
        return VOICE_REJECTED;
    }
    return VOICE_OK;
}

int voice_herdr_input(voice_herdr *herdr, const voice_target *target, const char *text, char *err, size_t err_cap) {
    (void)herdr;
    if (!target || !target->view.socket || !target->view.pane) {
        set_err(err, err_cap, "Selected Herdr pane is unavailable");
        return VOICE_REJECTED;
    }
    char start_before[VOICE_SHORT_MAX];
    start_before[0] = '\0';
    if (target->view.pid > 0) process_start_ticks(target->view.pid, start_before, sizeof start_before);
    unsigned char random[16];
    FILE *entropy = fopen("/dev/urandom", "rb");
    if (!entropy || fread(random, 1, sizeof random, entropy) != sizeof random) {
        for (size_t i = 0; i < sizeof random; i++) random[i] = (unsigned char)(i * 17 + 3);
    }
    if (entropy) fclose(entropy);
    char id[33];
    for (int i = 0; i < 16; i++) sprintf(id + i * 2, "%02x", random[i]);
    yyjson_mut_doc *mut = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(mut);
    yyjson_mut_doc_set_root(mut, obj);
    yyjson_mut_obj_add_strncpy(mut, obj, "id", id, 32);
    yyjson_mut_obj_add_strcpy(mut, obj, "method", "pane.send_input");
    yyjson_mut_val *params = yyjson_mut_obj(mut);
    yyjson_mut_obj_add_val(mut, obj, "params", params);
    yyjson_mut_obj_add_strncpy(mut, params, "pane_id", target->view.pane, strlen(target->view.pane));
    yyjson_mut_obj_add_strncpy(mut, params, "text", text ? text : "", text ? strlen(text) : 0);
    yyjson_mut_obj_add_val(mut, params, "keys", yyjson_mut_arr(mut));
    yyjson_doc *request = yyjson_mut_doc_imut_copy(mut, NULL);
    yyjson_mut_doc_free(mut);
    if (!request) return VOICE_REJECTED;
    ipc_unix_request call = {
        .socket_path = target->view.socket,
        .request = request,
        .deadline_ms = 8000,
        .max_response = 1024 * 1024,
    };
    int sent = 0;
    yyjson_doc *response = NULL;
    int rc = ipc_unix_json(&call, &sent, &response, err, err_cap);
    yyjson_doc_free(request);
    if (rc != IPC_OK) {
        yyjson_doc_free(response);
        if (!sent) {
            set_err(err, err_cap, "Selected Herdr pane is unavailable");
            return VOICE_REJECTED;
        }
        set_err(err, err_cap, "Could not confirm paste; check the selected Pi prompt and send it there if the dictation arrived");
        return VOICE_UNCERTAIN;
    }
    yyjson_val *root = yyjson_doc_get_root(response);
    const char *response_id = json_str(root, "id");
    if (!response_id || strcmp(response_id, id) != 0) {
        yyjson_doc_free(response);
        set_err(err, err_cap, "Could not confirm paste; check the selected Pi prompt and send it there if the dictation arrived");
        return VOICE_UNCERTAIN;
    }
    if (yyjson_obj_get(root, "error")) {
        yyjson_doc_free(response);
        set_err(err, err_cap, "Could not paste into the selected Pi pane");
        return VOICE_REJECTED;
    }
    yyjson_doc_free(response);
    if (target->view.pid > 0) {
        char start_after[VOICE_SHORT_MAX];
        if (process_start_ticks(target->view.pid, start_after, sizeof start_after) != 0 ||
            strcmp(start_after, start_before) != 0) {
            set_err(err, err_cap, "Could not confirm paste; check the selected Pi prompt and send it there if the dictation arrived");
            return VOICE_UNCERTAIN;
        }
    }
    return VOICE_OK;
}

int voice_herdr_validate_target(voice_herdr *herdr, const voice_target *target, yyjson_doc **agent, char *err, size_t err_cap) {
    if (agent) *agent = NULL;
    if (herdr_start_ok(target, err, err_cap) != 0) return VOICE_REJECTED;
    const char *info_args[] = {"pane", "process-info", "--pane", target->view.pane};
    yyjson_doc *info_doc = NULL;
    int rc = voice_herdr_request(herdr, target, info_args, 4, 0, &info_doc, err, err_cap);
    if (rc != VOICE_OK) return rc;
    yyjson_val *info = yyjson_obj_get(yyjson_doc_get_root(info_doc), "process_info");
    yyjson_val *procs = info && yyjson_is_obj(info) ? yyjson_obj_get(info, "foreground_processes") : NULL;
    int found = 0;
    if (procs && yyjson_is_arr(procs)) {
        size_t n = yyjson_arr_size(procs);
        for (size_t i = 0; i < n; i++) {
            yyjson_val *proc = yyjson_arr_get(procs, i);
            yyjson_val *pid = proc ? yyjson_obj_get(proc, "pid") : NULL;
            const char *name = proc ? json_str(proc, "name") : NULL;
            if (pid && yyjson_is_int(pid) && yyjson_get_int(pid) == target->view.pid && name &&
                target->view.harness && strstr(name, target->view.harness)) found = 1;
        }
    }
    yyjson_doc_free(info_doc);
    if (!found) {
        set_err(err, err_cap, "The selected agent process is no longer in the foreground");
        return VOICE_REJECTED;
    }
    char pane[VOICE_TEXT_MAX];
    snprintf(pane, sizeof pane, "%s", target->view.pane ? target->view.pane : "");
    const char *agent_args[] = {"agent", "get", pane};
    yyjson_doc *agent_doc = NULL;
    rc = voice_herdr_request(herdr, target, agent_args, 3, 0, &agent_doc, err, err_cap);
    if (rc != VOICE_OK) return rc;
    yyjson_val *body = yyjson_obj_get(yyjson_doc_get_root(agent_doc), "agent");
    const char *kind = body ? json_str(body, "agent") : NULL;
    if (!kind || !target->view.harness || strcmp(kind, target->view.harness) != 0) {
        char message[160];
        snprintf(message, sizeof message, "The selected pane is not running %s", target->view.harness ? target->view.harness : "");
        set_err(err, err_cap, message);
        yyjson_doc_free(agent_doc);
        return VOICE_REJECTED;
    }
    yyjson_doc *owned = doc_from_val(body);
    yyjson_doc_free(agent_doc);
    if (agent) *agent = owned;
    else yyjson_doc_free(owned);
    return owned ? VOICE_OK : VOICE_REJECTED;
}

int voice_herdr_validate(voice_herdr *herdr, const voice_target *target, char *err, size_t err_cap) {
    yyjson_doc *agent = NULL;
    int rc = voice_herdr_validate_target(herdr, target, &agent, err, err_cap);
    if (rc != VOICE_OK) return rc;
    const char *state = json_str(yyjson_doc_get_root(agent), "agent_status");
    int ready = state && (strcmp(state, "idle") == 0 || strcmp(state, "done") == 0);
    if (!ready) {
        char message[192];
        snprintf(message, sizeof message, "Selected agent is %s; finish its current interaction first", state ? state : "not ready");
        set_err(err, err_cap, message);
        yyjson_doc_free(agent);
        return VOICE_REJECTED;
    }
    yyjson_doc_free(agent);
    return VOICE_OK;
}

int voice_herdr_insert_guarded(
    voice_herdr *herdr, const voice_target *target, const char *text, atomic_int *cancelled, char *err, size_t err_cap
) {
    int rc = voice_herdr_validate(herdr, target, err, err_cap);
    if (rc != VOICE_OK) return rc;
    if (cancelled_now(cancelled)) {
        set_err(err, err_cap, "Voice delivery was cancelled");
        return VOICE_CANCELLED;
    }
    return voice_herdr_input(herdr, target, text, err, err_cap);
}

int voice_herdr_submit_guarded(
    voice_herdr *herdr, const voice_target *target, atomic_int *cancelled, int allow_edited, char *err, size_t err_cap
) {
    (void)allow_edited;
    int rc = voice_herdr_validate(herdr, target, err, err_cap);
    if (rc != VOICE_OK) return rc;
    if (cancelled_now(cancelled)) {
        set_err(err, err_cap, "Voice delivery was cancelled");
        return VOICE_CANCELLED;
    }
    const char *args[] = {"agent", "prompt", target->view.pane, " "};
    return voice_herdr_request(herdr, target, args, 4, 1, NULL, err, err_cap);
}

voice_terminal *voice_terminal_new(void) {
    voice_terminal *terminal = calloc(1, sizeof *terminal);
    if (!terminal) return NULL;
    terminal->pi = voice_pi_new();
    terminal->herdr = voice_herdr_new(NULL);
    if (!terminal->pi || !terminal->herdr) {
        voice_terminal_free(terminal);
        return NULL;
    }
    return terminal;
}

void voice_terminal_free(voice_terminal *terminal) {
    if (!terminal) return;
    voice_pi_free(terminal->pi);
    voice_herdr_free(terminal->herdr);
    free(terminal);
}

voice_pi *voice_terminal_pi(voice_terminal *terminal) { return terminal ? terminal->pi : NULL; }
voice_herdr *voice_terminal_herdr(voice_terminal *terminal) { return terminal ? terminal->herdr : NULL; }

void voice_terminal_set_herdr_argv0(voice_terminal *terminal, const char *argv0) {
    if (!terminal) return;
    voice_herdr *replacement = voice_herdr_new(argv0);
    if (!replacement) return;
    voice_herdr_free(terminal->herdr);
    terminal->herdr = replacement;
}

static int terminal_adapter(const voice_target *target, char *err, size_t err_cap) {
    char start[VOICE_SHORT_MAX];
    if (!target || !target->view.start || target->view.pid <= 0 ||
        process_start_ticks(target->view.pid, start, sizeof start) != 0 || strcmp(start, target->view.start) != 0) {
        set_err(err, err_cap, "Selected voice process has exited");
        return -1;
    }
    if (!target->view.harness || (strcmp(target->view.harness, "pi") != 0 && strcmp(target->view.harness, "qwen-pi") != 0)) {
        set_err(err, err_cap, "Only Pi sessions support voice");
        return -1;
    }
    return 0;
}

int voice_terminal_validate_target(
    voice_terminal *terminal, const voice_target *target, yyjson_doc **status, char *err, size_t err_cap
) {
    if (terminal_adapter(target, err, err_cap) != 0) return VOICE_REJECTED;
    return voice_pi_validate_target(terminal->pi, target, status, err, err_cap);
}

int voice_terminal_validate(voice_terminal *terminal, const voice_target *target, char *err, size_t err_cap) {
    if (terminal_adapter(target, err, err_cap) != 0) return VOICE_REJECTED;
    return voice_pi_validate(terminal->pi, target, err, err_cap);
}

int voice_terminal_activity(
    voice_terminal *terminal, const voice_target *target, char *state, size_t state_cap, char *err, size_t err_cap
) {
    if (state && state_cap) state[0] = '\0';
    yyjson_doc *status = NULL;
    int rc = voice_terminal_validate_target(terminal, target, &status, err, err_cap);
    if (rc != VOICE_OK) return rc;
    yyjson_val *root = yyjson_doc_get_root(status);
    const char *value = json_str(root, "agent_status");
    if (!value) value = json_str(root, "state");
    if (!value) value = "unknown";
    if (strcmp(value, "done") == 0) value = "idle";
    if (strcmp(value, "working") != 0 && strcmp(value, "blocked") != 0 && strcmp(value, "idle") != 0) value = "unknown";
    if (state && state_cap) snprintf(state, state_cap, "%s", value);
    yyjson_doc_free(status);
    return VOICE_OK;
}

int voice_terminal_insert_guarded(
    voice_terminal *terminal, const voice_target *target, const char *text, atomic_int *cancelled, char *err, size_t err_cap
) {
    if (terminal_adapter(target, err, err_cap) != 0) return VOICE_REJECTED;
    return voice_pi_insert_guarded(terminal->pi, target, text, cancelled, err, err_cap);
}

int voice_terminal_submit_guarded(
    voice_terminal *terminal, const voice_target *target, atomic_int *cancelled, int allow_edited, char *err, size_t err_cap
) {
    if (terminal_adapter(target, err, err_cap) != 0) return VOICE_REJECTED;
    return voice_pi_submit_guarded(terminal->pi, target, cancelled, allow_edited, err, err_cap);
}
