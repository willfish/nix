#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "text.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *voice_envelope_ok(const char *object_json) {
    if (!object_json || strcmp(object_json, "{}") == 0) {
        char *out = strdup("{\"ok\":true}");
        return out;
    }
    size_t n = strlen(object_json);
    if (n < 2 || object_json[0] != '{') return voice_envelope_error("Invalid voice response");
    char *out = malloc(n + 16);
    if (!out) return NULL;
    memcpy(out, "{\"ok\":true,", 11);
    memcpy(out + 11, object_json + 1, n);
    return out;
}

char *voice_envelope_error(const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", 0);
    yyjson_mut_obj_add_str(doc, root, "error", message && message[0] ? message : "Voice command failed");
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return text;
}

char *voice_envelope_fields(const char *key, const char *object_json, int bool_value, int has_bool) {
    yyjson_doc *inner = object_json ? yyjson_read(object_json, strlen(object_json), 0) : NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        yyjson_doc_free(inner);
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", 1);
    if (has_bool) yyjson_mut_obj_add_bool(doc, root, key, bool_value);
    else if (inner && yyjson_is_obj(yyjson_doc_get_root(inner))) {
        yyjson_mut_val *copied = yyjson_val_mut_copy(doc, yyjson_doc_get_root(inner));
        yyjson_mut_obj_add_val(doc, root, key, copied);
    } else {
        yyjson_mut_obj_add_null(doc, root, key);
    }
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(inner);
    return text;
}

int voice_tone_ok(const char *tone) {
    static const char *ok[] = {"red", "yellow", "green", "orange", "teal", "accent", "muted", NULL};
    if (!tone) return 0;
    for (int i = 0; ok[i]; i++) if (strcmp(ok[i], tone) == 0) return 1;
    return 0;
}

void voice_public_text(const char *input, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!input) return;
    size_t n = 0;
    int space = 1;
    for (const unsigned char *p = (const unsigned char *)input; *p && n + 1 < cap && n < VOICE_NOTICE_MAX; p++) {
        unsigned char c = *p;
        int printable = c == '\t' || c == '\n' || c == '\r' || (c >= 32 && c != 127);
        if (!printable) continue;
        if (c == '\t' || c == '\n' || c == '\r' || c == ' ') {
            if (space) continue;
            if (n + 1 >= cap) break;
            out[n++] = ' ';
            space = 1;
            continue;
        }
        out[n++] = (char)c;
        space = 0;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void add_str(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, const char *value, int present) {
    if (!present || !value) yyjson_mut_obj_add_null(doc, obj, key);
    else yyjson_mut_obj_add_strcpy(doc, obj, key, value);
}

static yyjson_mut_val *target_json(yyjson_mut_doc *doc, const target_data *t) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, obj, "pane", t->pane);
    yyjson_mut_obj_add_strcpy(doc, obj, "socket", t->socket);
    if (t->has_socket_instance) {
        yyjson_mut_obj_add_uint(doc, obj, "socket_device", t->socket_device);
        yyjson_mut_obj_add_uint(doc, obj, "socket_inode", t->socket_inode);
    }
    yyjson_mut_obj_add_int(doc, obj, "pid", t->pid);
    if (t->has_start) yyjson_mut_obj_add_strcpy(doc, obj, "start", t->start);
    yyjson_mut_obj_add_strcpy(doc, obj, "harness", t->harness[0] ? t->harness : "pi");
    if (t->has_session) {
        if (t->session_null) yyjson_mut_obj_add_null(doc, obj, "session");
        else yyjson_mut_obj_add_strcpy(doc, obj, "session", t->session);
    }
    if (t->has_bridge) yyjson_mut_obj_add_strcpy(doc, obj, "bridge_id", t->bridge_id);
    if (t->has_activation) yyjson_mut_obj_add_sint(doc, obj, "activation", t->activation);
    if (t->has_team_child) yyjson_mut_obj_add_bool(doc, obj, "team_child", t->team_child);
    if (t->has_model) {
        if (t->model_is_dict) {
            yyjson_mut_val *model = yyjson_mut_obj(doc);
            if (t->model_id[0]) yyjson_mut_obj_add_strcpy(doc, model, "id", t->model_id);
            if (t->model_name[0]) yyjson_mut_obj_add_strcpy(doc, model, "name", t->model_name);
            yyjson_mut_obj_add_val(doc, obj, "model", model);
        } else yyjson_mut_obj_add_strcpy(doc, obj, "model", t->model);
    }
    if (t->has_thinking) yyjson_mut_obj_add_strcpy(doc, obj, "thinking", t->thinking);
    if (t->has_adapter) {
        yyjson_mut_obj_add_strcpy(doc, obj, "adapter_socket", t->adapter_socket);
        yyjson_mut_obj_add_uint(doc, obj, "adapter_device", t->adapter_device);
        yyjson_mut_obj_add_uint(doc, obj, "adapter_inode", t->adapter_inode);
    }
    if (t->managed) yyjson_mut_obj_add_bool(doc, obj, "managed", 1);
    if (t->token[0]) yyjson_mut_obj_add_strcpy(doc, obj, "token", t->token);
    return obj;
}

static int copy_field(char *dst, size_t cap, const char *src) {
    if (!src) {
        if (cap) dst[0] = 0;
        return 0;
    }
    if (strlen(src) >= cap) return -1;
    memcpy(dst, src, strlen(src) + 1);
    return 0;
}

int controller_parse_target(yyjson_val *obj, target_data *out, char *err, size_t cap) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(obj)) {
        snprintf(err, cap, "Invalid voice target");
        return -1;
    }
    yyjson_val *pane = yyjson_obj_get(obj, "pane");
    yyjson_val *socket = yyjson_obj_get(obj, "socket");
    if (!yyjson_is_str(pane) || !yyjson_is_str(socket)) {
        snprintf(err, cap, "Invalid voice target");
        return -1;
    }
    if (copy_field(out->pane, sizeof out->pane, yyjson_get_str(pane)) ||
        copy_field(out->socket, sizeof out->socket, yyjson_get_str(socket))) {
        snprintf(err, cap, "Invalid voice target");
        return -1;
    }
    yyjson_val *pid = yyjson_obj_get(obj, "pid");
    if (pid && !yyjson_is_null(pid)) {
        if (!yyjson_is_int(pid) || yyjson_is_real(pid)) {
            snprintf(err, cap, "Invalid Pi process or activation");
            return -1;
        }
        int64_t value = yyjson_get_sint(pid);
        if (value <= 0 || value > INT_MAX) {
            snprintf(err, cap, "Invalid Pi process or activation");
            return -1;
        }
        out->pid = (int)value;
    }
    yyjson_val *start = yyjson_obj_get(obj, "start");
    if (yyjson_is_str(start)) {
        if (copy_field(out->start, sizeof out->start, yyjson_get_str(start))) return -1;
        out->has_start = 1;
    }
    yyjson_val *harness = yyjson_obj_get(obj, "harness");
    if (yyjson_is_str(harness)) {
        if (copy_field(out->harness, sizeof out->harness, yyjson_get_str(harness))) return -1;
    } else {
        snprintf(out->harness, sizeof out->harness, "pi");
    }
    yyjson_val *session = yyjson_obj_get(obj, "session");
    if (session) {
        out->has_session = 1;
        if (yyjson_is_null(session)) out->session_null = 1;
        else if (yyjson_is_str(session)) {
            if (copy_field(out->session, sizeof out->session, yyjson_get_str(session))) return -1;
        }
    }
    yyjson_val *bridge = yyjson_obj_get(obj, "bridge_id");
    if (yyjson_is_str(bridge)) {
        if (copy_field(out->bridge_id, sizeof out->bridge_id, yyjson_get_str(bridge))) return -1;
        out->has_bridge = 1;
    }
    yyjson_val *activation = yyjson_obj_get(obj, "activation");
    if (yyjson_is_int(activation)) {
        out->has_activation = 1;
        out->activation = yyjson_get_sint(activation);
    }
    yyjson_val *team = yyjson_obj_get(obj, "team_child");
    if (yyjson_is_bool(team)) {
        out->has_team_child = 1;
        out->team_child = yyjson_get_bool(team);
    }
    yyjson_val *model = yyjson_obj_get(obj, "model");
    if (yyjson_is_str(model)) {
        if (copy_field(out->model, sizeof out->model, yyjson_get_str(model))) return -1;
        out->has_model = 1;
    } else if (yyjson_is_obj(model)) {
        out->has_model = 1;
        out->model_is_dict = 1;
        if (yyjson_is_str(yyjson_obj_get(model, "id"))
            && copy_field(out->model_id, sizeof out->model_id, yyjson_get_str(yyjson_obj_get(model, "id")))) return -1;
        if (yyjson_is_str(yyjson_obj_get(model, "name"))
            && copy_field(out->model_name, sizeof out->model_name, yyjson_get_str(yyjson_obj_get(model, "name")))) return -1;
    }
    yyjson_val *thinking = yyjson_obj_get(obj, "thinking");
    if (yyjson_is_str(thinking)) {
        if (copy_field(out->thinking, sizeof out->thinking, yyjson_get_str(thinking))) return -1;
        out->has_thinking = 1;
    }
    yyjson_val *adapter = yyjson_obj_get(obj, "adapter_socket");
    if (yyjson_is_str(adapter)) {
        if (copy_field(out->adapter_socket, sizeof out->adapter_socket, yyjson_get_str(adapter))) return -1;
        out->has_adapter = 1;
    }
    yyjson_val *managed = yyjson_obj_get(obj, "managed");
    out->managed = yyjson_is_bool(managed) && yyjson_get_bool(managed);
    yyjson_val *token = yyjson_obj_get(obj, "token");
    if (yyjson_is_str(token)) {
        if (copy_field(out->token, sizeof out->token, yyjson_get_str(token))) return -1;
    }
    if (yyjson_is_uint(yyjson_obj_get(obj, "socket_device"))) {
        out->has_socket_instance = 1;
        out->socket_device = yyjson_get_uint(yyjson_obj_get(obj, "socket_device"));
        out->socket_inode = yyjson_get_uint(yyjson_obj_get(obj, "socket_inode"));
    }
    if (out->has_adapter && yyjson_is_uint(yyjson_obj_get(obj, "adapter_device"))) {
        out->adapter_device = yyjson_get_uint(yyjson_obj_get(obj, "adapter_device"));
        out->adapter_inode = yyjson_get_uint(yyjson_obj_get(obj, "adapter_inode"));
    }
    return 0;
}

static void add_turns(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, char **turns, size_t count) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    char **sorted = calloc(count ? count : 1, sizeof *sorted);
    size_t n = 0;
    for (size_t i = 0; i < count; i++) if (turns && turns[i]) sorted[n++] = turns[i];
    for (size_t i = 1; i < n; i++) {
        char *tmp = sorted[i];
        size_t j = i;
        while (j > 0 && strcmp(sorted[j - 1], tmp) > 0) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = tmp;
    }
    for (size_t i = 0; i < n; i++) yyjson_mut_arr_add_strcpy(doc, arr, sorted[i]);
    free(sorted);
    yyjson_mut_obj_add_val(doc, obj, key, arr);
}

char *controller_selection_json(voice_controller *app) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (app->has_token) yyjson_mut_obj_add_strcpy(doc, root, "token", app->token);
    else yyjson_mut_obj_add_null(doc, root, "token");
    yyjson_mut_obj_add_bool(doc, root, "show_team", app->show_team);
    yyjson_mut_obj_add_bool(doc, root, "selection_initialized", app->selection_initialized);
    yyjson_mut_obj_add_bool(doc, root, "selection_explicit", app->selection_explicit);
    if (app->has_reconnect) {
        yyjson_mut_val *pane = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, pane, "socket_path", app->reconnect_path);
        yyjson_mut_obj_add_uint(doc, pane, "socket_device", app->reconnect_pane.socket_device);
        yyjson_mut_obj_add_uint(doc, pane, "socket_inode", app->reconnect_pane.socket_inode);
        yyjson_mut_obj_add_strcpy(doc, pane, "pane_id", app->reconnect_pane_id);
        yyjson_mut_obj_add_val(doc, root, "reconnect_pane", pane);
    } else {
        yyjson_mut_obj_add_null(doc, root, "reconnect_pane");
    }
    yyjson_mut_obj_add_bool(doc, root, "reconnect_explicit", app->reconnect_explicit);
    Fences fences = {0};
    yyjson_mut_val *fence = yyjson_mut_obj(doc);
    yyjson_mut_val *high = yyjson_mut_arr(doc);
    yyjson_mut_val *retired = yyjson_mut_arr(doc);
    if (app->attachments && registry_fences(app->attachments, &fences) == 0) {
        for (size_t i = 0; i < fences.highwater_count; i++) {
            yyjson_mut_val *row = yyjson_mut_arr(doc);
            yyjson_mut_arr_add_int(doc, row, fences.highwater[i].pid);
            yyjson_mut_arr_add_strcpy(doc, row, fences.highwater[i].process_start ? fences.highwater[i].process_start : "");
            yyjson_mut_arr_add_sint(doc, row, fences.highwater[i].ordinal);
            yyjson_mut_arr_add_strcpy(doc, row, fences.highwater[i].bridge_id ? fences.highwater[i].bridge_id : "");
            yyjson_mut_arr_add_val(high, row);
        }
        for (size_t i = 0; i < fences.retired_count; i++) {
            yyjson_mut_val *row = yyjson_mut_arr(doc);
            yyjson_mut_arr_add_int(doc, row, fences.retired[i].pid);
            yyjson_mut_arr_add_strcpy(doc, row, fences.retired[i].process_start ? fences.retired[i].process_start : "");
            yyjson_mut_arr_add_strcpy(doc, row, fences.retired[i].bridge_id ? fences.retired[i].bridge_id : "");
            yyjson_mut_arr_add_val(retired, row);
        }
    }
    fences_free(&fences);
    yyjson_mut_obj_add_val(doc, fence, "highwater", high);
    yyjson_mut_obj_add_val(doc, fence, "retired", retired);
    yyjson_mut_obj_add_val(doc, root, "attachment_fences", fence);
    if (app->target) yyjson_mut_obj_add_val(doc, root, "target", target_json(doc, &app->target->data));
    else yyjson_mut_obj_add_null(doc, root, "target");
    if (app->has_thread) yyjson_mut_obj_add_strcpy(doc, root, "thread", app->thread);
    else yyjson_mut_obj_add_null(doc, root, "thread");
    add_turns(doc, root, "turns", app->turns, app->turn_count);
    yyjson_mut_val *sessions = yyjson_mut_obj(doc);
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
        session_entry *entry = &app->sessions[i];
        if (!entry->used || !entry->target) continue;
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, row, "target", target_json(doc, &entry->target->data));
        if (entry->has_thread) yyjson_mut_obj_add_strcpy(doc, row, "thread", entry->thread);
        else yyjson_mut_obj_add_null(doc, row, "thread");
        if (entry->has_candidate) yyjson_mut_obj_add_strcpy(doc, row, "candidate", entry->candidate);
        else yyjson_mut_obj_add_null(doc, row, "candidate");
        yyjson_mut_val *excluded = yyjson_mut_arr(doc);
        for (size_t e = 0; e < entry->excluded_count; e++) yyjson_mut_arr_add_strcpy(doc, excluded, entry->excluded[e]);
        yyjson_mut_obj_add_val(doc, row, "excluded", excluded);
        add_turns(doc, row, "turns", entry->turns, entry->turn_count);
        yyjson_mut_obj_add_val(doc, sessions, entry->token, row);
    }
    yyjson_mut_obj_add_val(doc, root, "sessions", sessions);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return text;
}

static void copy_obj(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key, yyjson_val *audio, const char *audio_key) {
    yyjson_val *value = audio ? yyjson_obj_get(audio, audio_key ? audio_key : key) : NULL;
    if (value && (yyjson_is_obj(value) || yyjson_is_arr(value) || yyjson_is_str(value))) {
        yyjson_mut_obj_add_val(doc, root, key, yyjson_val_mut_copy(doc, value));
    } else if (strcmp(key, "voices") == 0 || strcmp(key, "speech_backends") == 0 || strcmp(key, "stt_backends") == 0) {
        yyjson_mut_obj_add_val(doc, root, key, yyjson_mut_obj(doc));
    } else {
        yyjson_mut_obj_add_strcpy(doc, root, key, "");
    }
}

char *controller_status_json(voice_controller *app) {
    controller_refresh_activity(app);
    char *audio_text = NULL;
    if (app->deps.audio.status_json) app->deps.audio.status_json(app->deps.audio.user, &audio_text);
    yyjson_doc *audio_doc = audio_text ? yyjson_read(audio_text, strlen(audio_text), 0) : NULL;
    yyjson_val *audio = audio_doc ? yyjson_doc_get_root(audio_doc) : NULL;
    if (audio && !yyjson_is_obj(audio)) audio = NULL;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        free(audio_text);
        yyjson_doc_free(audio_doc);
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    pthread_mutex_lock(&app->state);
    yyjson_mut_obj_add_bool(doc, root, "show_team", app->show_team);
    yyjson_mut_obj_add_bool(doc, root, "selection_explicit", app->selection_explicit);
    add_str(doc, root, "recording_label", app->recording_label, app->recording_label != NULL);
    add_str(doc, root, "pane", app->target ? app->target->data.pane : NULL, app->target != NULL);
    session_entry *selected = app->has_token ? controller_find(app, app->token) : NULL;
    const char *connection = "unselected";
    if (selected && selected->has_connection) connection = selected->connection_state;
    else if (app->target) connection = "ready";
    else if (app->has_reconnect) connection = "reconnecting";
    yyjson_mut_obj_add_strcpy(doc, root, "connection_state", connection);
    yyjson_mut_obj_add_bool(doc, root, "retained", app->retained != NULL);
    add_str(doc, root, "retained_source", app->retained ? app->retained->source_label : NULL, app->retained != NULL);
    add_str(doc, root, "harness", app->target ? (app->target->data.harness[0] ? app->target->data.harness : "pi") : NULL, app->target != NULL);
    const char *agent = selected ? selected->agent_state : "unknown";
    if (!agent[0]) agent = "unknown";
    yyjson_mut_obj_add_strcpy(doc, root, "agent_state", agent);
    yyjson_mut_obj_add_bool(doc, root, "responding", strcmp(agent, "working") == 0);
    const char *thread = app->has_thread && app->thread[0] ? app->thread : "Awaiting conversation";
    char session_label[49];
    size_t thread_len = strlen(thread);
    if (thread_len > 48) thread_len = 48;
    memcpy(session_label, thread, thread_len);
    session_label[thread_len] = 0;
    yyjson_mut_obj_add_strcpy(doc, root, "session_label", session_label);
    int rebind = 0;
    if (selected && selected->has_candidate) {
        int known = app->has_thread && strcmp(selected->candidate, app->thread) == 0;
        for (size_t i = 0; !known && i < selected->excluded_count; i++)
            if (strcmp(selected->excluded[i], selected->candidate) == 0) known = 1;
        rebind = !known;
    }
    yyjson_mut_obj_add_bool(doc, root, "rebind_needed", rebind);
    yyjson_mut_val *sessions = yyjson_mut_arr(doc);
    LabelEntry raw_rows[CTRL_MAX_SESSIONS];
    size_t raw_count = 0;
    SocketKey snap_keys[CTRL_MAX_SESSIONS];
    memset(raw_rows, 0, sizeof raw_rows);
    memset(snap_keys, 0, sizeof snap_keys);
    size_t visible = 0;
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
        session_entry *entry = &app->sessions[i];
        if (!entry->used || !entry->target) continue;
        int team = entry->target->data.team_child;
        int is_sel = app->has_token && strcmp(entry->token, app->token) == 0;
        if (!app->show_team && team && !is_sel) continue;
        yyjson_mut_val *row = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, row, "token", entry->token);
        yyjson_mut_obj_add_strcpy(doc, row, "pane", entry->target->data.pane);
        if (entry->target->data.has_model && entry->target->data.model_is_dict) {
            yyjson_mut_val *model = yyjson_mut_obj(doc);
            if (entry->target->data.model_id[0]) yyjson_mut_obj_add_strcpy(doc, model, "id", entry->target->data.model_id);
            if (entry->target->data.model_name[0]) yyjson_mut_obj_add_strcpy(doc, model, "name", entry->target->data.model_name);
            yyjson_mut_obj_add_val(doc, row, "model", model);
        } else if (entry->target->data.has_model) yyjson_mut_obj_add_strcpy(doc, row, "model", entry->target->data.model);
        else yyjson_mut_obj_add_null(doc, row, "model");
        if (entry->target->data.has_thinking) yyjson_mut_obj_add_strcpy(doc, row, "thinking", entry->target->data.thinking);
        else yyjson_mut_obj_add_null(doc, row, "thinking");
        yyjson_mut_obj_add_bool(doc, row, "selected", is_sel);
        yyjson_mut_obj_add_strcpy(doc, row, "harness", entry->target->data.harness[0] ? entry->target->data.harness : "pi");
        yyjson_mut_obj_add_bool(doc, row, "team_child", team);
        yyjson_mut_obj_add_strcpy(doc, row, "connection_state", entry->has_connection ? entry->connection_state : "ready");
        const char *id = entry->has_thread && entry->thread[0] ? entry->thread : entry->target->data.pane;
        yyjson_mut_obj_add_strcpy(doc, row, "id", id);
        const char *harness = entry->target->data.harness[0] ? entry->target->data.harness : "pi";
        int pi = strcmp(harness, "pi") == 0 || strcmp(harness, "qwen-pi") == 0;
        const char *tail = pi ? entry->target->data.pane : (entry->has_thread && entry->thread[0] ? entry->thread : entry->target->data.pane);
        char label[CTRL_PANE + 64];
        snprintf(label, sizeof label, "%s: %s", harness, tail);
        yyjson_mut_obj_add_strcpy(doc, row, "label", label);
        if (entry->has_socket_key || entry->target->data.managed) {
            LabelEntry *raw = &raw_rows[raw_count];
            raw->harness = strdup(harness);
            raw->pane = strdup(entry->target->data.pane);
            raw->token = strdup(entry->token);
            raw->id = strdup(id);
            raw->label = strdup(label);
            raw->selected = is_sel;
            raw->team_child = team;
            raw->has_model = entry->target->data.has_model;
            raw->model_is_dict = entry->target->data.model_is_dict;
            if (entry->target->data.has_model && !entry->target->data.model_is_dict) raw->model = strdup(entry->target->data.model);
            if (entry->target->data.model_id[0]) raw->model_id = strdup(entry->target->data.model_id);
            if (entry->target->data.model_name[0]) raw->model_name = strdup(entry->target->data.model_name);
            if (entry->target->data.has_thinking) raw->thinking = strdup(entry->target->data.thinking);
            raw->has_socket_key = 1;
            if (entry->target->data.managed) {
                socket_key_init(&raw->socket_key, entry->target->data.socket, entry->target->data.socket_device, entry->target->data.socket_inode);
            } else if (entry->has_socket_key) {
                socket_key_init(&raw->socket_key, entry->socket_key.path, entry->socket_key.device, entry->socket_key.inode);
            }
            raw_count++;
        }
        yyjson_mut_arr_add_val(sessions, row);
        visible++;
    }
    yyjson_mut_obj_add_val(doc, root, "sessions", sessions);
    yyjson_mut_val *mic = yyjson_mut_obj(doc);
    yyjson_val *audio_mic = audio ? yyjson_obj_get(audio, "microphone") : NULL;
    if (yyjson_is_obj(audio_mic)) {
        yyjson_val *k, *v;
        yyjson_obj_iter iter = yyjson_obj_iter_with(audio_mic);
        while ((k = yyjson_obj_iter_next(&iter))) {
            v = yyjson_obj_iter_get_val(k);
            yyjson_mut_obj_add_val(doc, mic, yyjson_get_str(k), yyjson_val_mut_copy(doc, v));
        }
    }
    int clipping = app->capture && app->capture->clipping ? app->capture->clipping(app->capture) : 0;
    yyjson_mut_obj_add_bool(doc, mic, "clipping", clipping);
    yyjson_mut_obj_add_val(doc, root, "microphone", mic);
    int session_count = 0;
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) if (app->sessions[i].used) session_count++;
    yyjson_val *backends = audio ? yyjson_obj_get(audio, "backends") : NULL;
    const char *models = NULL;
    if (session_count) {
        models = "loading";
        if (yyjson_is_obj(backends)) {
            int any = 0, ready = 1, bad = 0;
            yyjson_val *k, *v;
            yyjson_obj_iter iter = yyjson_obj_iter_with(backends);
            while ((k = yyjson_obj_iter_next(&iter))) {
                v = yyjson_obj_iter_get_val(k);
                any = 1;
                const char *state = yyjson_is_str(v) ? yyjson_get_str(v) : "";
                if (strcmp(state, "error") == 0) bad = 1;
                if (strcmp(state, "ready") != 0) ready = 0;
            }
            if (bad) models = "unavailable";
            else if (any && ready) models = "ready";
        }
    }
    add_str(doc, root, "models", models, models != NULL);
    char model_error[1024] = {0};
    yyjson_val *errors = audio ? yyjson_obj_get(audio, "backend_errors") : NULL;
    if (yyjson_is_obj(errors)) {
        yyjson_val *k, *v;
        yyjson_obj_iter iter = yyjson_obj_iter_with(errors);
        while ((k = yyjson_obj_iter_next(&iter))) {
            v = yyjson_obj_iter_get_val(k);
            if (!yyjson_is_str(v)) continue;
            if (model_error[0]) strncat(model_error, "; ", sizeof model_error - strlen(model_error) - 1);
            strncat(model_error, yyjson_get_str(v), sizeof model_error - strlen(model_error) - 1);
        }
    }
    yyjson_mut_obj_add_strcpy(doc, root, "model_error", model_error);
    yyjson_mut_obj_add_bool(doc, root, "draft", app->draft);
    int edited = selected && (strcmp(selected->draft_state, "edited") == 0 || strcmp(selected->draft_state, "empty") == 0 || strcmp(selected->draft_state, "none") == 0);
    yyjson_mut_obj_add_bool(doc, root, "draft_edited", edited);
    add_str(doc, root, "reply", app->reply, app->reply != NULL);
    yyjson_mut_obj_add_bool(doc, root, "auto", app->auto_read);
    yyjson_mut_obj_add_bool(doc, root, "speech_available", app->speech_available);
    yyjson_mut_obj_add_bool(doc, root, "can_speak", app->target && !app->target->data.team_child);
    copy_obj(doc, root, "selected_voice", audio, NULL);
    copy_obj(doc, root, "voices", audio, NULL);
    copy_obj(doc, root, "speech_backend", audio, NULL);
    copy_obj(doc, root, "speech_backends", audio, NULL);
    copy_obj(doc, root, "selected_stt", audio, NULL);
    copy_obj(doc, root, "stt_backends", audio, NULL);
    yyjson_mut_obj_add_bool(doc, root, "recording", strcmp(app->phase, "recording") == 0);
    yyjson_mut_obj_add_bool(doc, root, "transcribing", strcmp(app->phase, "transcribing") == 0);
    yyjson_mut_obj_add_bool(doc, root, "speaking", app->speaking);
    yyjson_mut_obj_add_bool(doc, root, "audible", app->audible);
    yyjson_mut_obj_add_bool(doc, root, "queued", app->send_when_idle);
    yyjson_mut_obj_add_strcpy(doc, root, "phase", app->phase);
    add_str(doc, root, "error", app->error, app->error != NULL);
    yyjson_mut_obj_add_bool(doc, root, "pending", app->pending != NULL);
    yyjson_mut_obj_add_bool(doc, root, "retry", app->has_retry);
    double seconds = 0;
    if (app->has_record_started && strcmp(app->phase, "recording") == 0) {
        seconds = controller_now(app) - app->record_started;
        if (seconds < 0) seconds = 0;
    }
    yyjson_mut_obj_add_real(doc, root, "recording_seconds", seconds);
    double level = app->capture && app->capture->level ? app->capture->level(app->capture) : 0;
    yyjson_mut_obj_add_real(doc, root, "input_level", level);
    int osd = controller_recording_active(app) || controller_now(app) < app->osd_until;
    yyjson_mut_obj_add_bool(doc, root, "osd", osd);
    add_str(doc, root, "osd_message", app->osd_message, app->osd_message != NULL);
    add_str(doc, root, "osd_tone", app->has_osd_tone ? app->osd_tone : NULL, app->has_osd_tone);
    pthread_mutex_unlock(&app->state);

    if (app->deps.has_catalogue && raw_count) {
        SnapshotState *states[CTRL_MAX_SESSIONS] = {0};
        SocketKey keys[CTRL_MAX_SESSIONS];
        size_t nk = 0;
        for (size_t i = 0; i < raw_count; i++) {
            int seen = 0;
            for (size_t j = 0; j < nk; j++) if (socket_key_equal(&keys[j], &raw_rows[i].socket_key)) seen = 1;
            if (!seen) keys[nk++] = raw_rows[i].socket_key;
        }
        for (size_t i = 0; i < nk; i++) {
            if (app->deps.catalogue.read_snapshot)
                states[i] = app->deps.catalogue.read_snapshot(app->deps.catalogue.user,
                    keys[i].path, keys[i].device, keys[i].inode);
        }
        LabelEntry *labelled = NULL;
        size_t labelled_count = 0;
        if (labels_build(raw_rows, raw_count, keys, states, nk, &labelled, &labelled_count) == 0 && labelled) {
            size_t idx, max;
            yyjson_mut_val *row;
            yyjson_mut_arr_foreach(sessions, idx, max, row) {
                yyjson_mut_val *token = yyjson_mut_obj_get(row, "token");
                const char *tok = token ? yyjson_mut_get_str(token) : NULL;
                for (size_t i = 0; tok && i < labelled_count; i++) {
                    if (!labelled[i].token || strcmp(labelled[i].token, tok) != 0) continue;
                    if (labelled[i].label_present && labelled[i].label)
                        yyjson_mut_obj_remove_key(row, "label"), yyjson_mut_obj_add_strcpy(doc, row, "label", labelled[i].label);
                    if (labelled[i].full_label_present && labelled[i].full_label)
                        yyjson_mut_obj_add_strcpy(doc, row, "full_label", labelled[i].full_label);
                }
            }
            labels_entries_free(labelled, labelled_count);
        }
        for (size_t i = 0; i < nk; i++) labels_state_free(states[i]);
    }
    for (size_t i = 0; i < raw_count; i++) labels_entry_clear(&raw_rows[i]);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(audio_doc);
    free(audio_text);
    return text;
}
