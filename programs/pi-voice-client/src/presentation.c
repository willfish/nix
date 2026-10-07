#include "presentation.h"

#include <ctype.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

static char *xstrdup(const char *text) {
    size_t n;
    char *copy;
    if (!text) return NULL;
    n = strlen(text) + 1;
    copy = malloc(n);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    return copy;
}

static int fail(char **error, const char *message) {
    if (error && !*error) *error = xstrdup(message ? message : "Voice presentation failed");
    return -1;
}

static int utf8_next(const char *s, gunichar *cp, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    if (!u || !u[0]) return 0;
    if (u[0] < 0x80) {
        *cp = u[0];
        *len = 1;
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        *len = 2;
        return 1;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        *len = 3;
        return 1;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        *len = 4;
        return 1;
    }
    *cp = u[0];
    *len = 1;
    return 1;
}

static int py_printable(gunichar c) {
    if (c == ' ') return 1;
    switch (g_unichar_type(c)) {
    case G_UNICODE_CONTROL:
    case G_UNICODE_FORMAT:
    case G_UNICODE_SURROGATE:
    case G_UNICODE_PRIVATE_USE:
    case G_UNICODE_UNASSIGNED:
    case G_UNICODE_LINE_SEPARATOR:
    case G_UNICODE_PARAGRAPH_SEPARATOR:
    case G_UNICODE_SPACE_SEPARATOR:
        return 0;
    default:
        return 1;
    }
}

int voice_public_label(const char *value, size_t limit, char *out, size_t cap) {
    size_t used = 0;
    size_t cps = 0;
    int started = 0;
    int pending_space = 0;
    const char *cursor = value ? value : "";
    if (!out || cap == 0) return -1;
    out[0] = 0;
    while (*cursor && cps < limit) {
        gunichar cp = 0;
        size_t len = 0;
        char encoded[8];
        int n;
        if (!utf8_next(cursor, &cp, &len)) break;
        if (g_unichar_isspace(cp)) {
            if (started) pending_space = 1;
        } else if (py_printable(cp)) {
            if (pending_space && cps < limit) {
                if (used + 2 > cap) return -1;
                out[used++] = ' ';
                out[used] = 0;
                cps++;
                pending_space = 0;
            }
            if (cps < limit) {
                n = g_unichar_to_utf8(cp, encoded);
                if (n < 0 || used + (size_t)n + 1 > cap) return -1;
                memcpy(out + used, encoded, (size_t)n);
                used += (size_t)n;
                out[used] = 0;
                cps++;
            }
            started = 1;
        }
        cursor += len;
    }
    return 0;
}

static char *public_dup(const char *value, size_t limit) {
    char buf[8192];
    if (voice_public_label(value, limit, buf, sizeof buf)) {
        size_t cap = strlen(value ? value : "") + 8;
        char *big = malloc(cap > 64 ? cap : 64);
        if (!big) return NULL;
        if (voice_public_label(value, limit, big, cap)) {
            free(big);
            return xstrdup("");
        }
        return big;
    }
    return xstrdup(buf);
}

static const char *phase_of(const VoiceMenuStatus *status) {
    if (!status || !status->phase || !status->phase[0]) {
        if (status && status->phase && !status->phase[0]) return "";
        return "idle";
    }
    return status->phase;
}

int voice_menu_busy(const VoiceMenuStatus *status) {
    const char *phase = phase_of(status);
    return status && (status->preparing
        || strcmp(phase, "starting") == 0
        || strcmp(phase, "recording") == 0
        || strcmp(phase, "stopping") == 0
        || strcmp(phase, "transcribing") == 0);
}

static int pane_selected(const VoiceMenuStatus *status) {
    return status && status->pane && status->pane[0];
}

static const char *connection_of(const VoiceMenuStatus *status) {
    if (status && status->connection_state) return status->connection_state;
    return pane_selected(status) ? "ready" : "unselected";
}

static int selection_explicit(const VoiceMenuStatus *status) {
    if (!status || status->selection_explicit_missing) return 1;
    return status->selection_explicit;
}

static int speech_available(const VoiceMenuStatus *status) {
    if (!status || status->speech_available_missing) return 1;
    return status->speech_available;
}

static int can_speak(const VoiceMenuStatus *status) {
    if (!status || status->can_speak_missing) return 1;
    return status->can_speak;
}

static int auto_on(const VoiceMenuStatus *status) {
    return status && !status->auto_missing && status->auto_value;
}

static const char *or_text(const char *value, const char *fallback) {
    return value && value[0] ? value : fallback;
}

static char *harness_label(const VoiceMenuStatus *status) {
    char *raw = public_dup(or_text(status && status->harness ? status->harness : NULL, "Pi"), 30);
    if (!raw) return NULL;
    if (strcmp(raw, "pi") == 0) {
        free(raw);
        return xstrdup("Pi");
    }
    if (strcmp(raw, "qwen-pi") == 0) {
        free(raw);
        return xstrdup("Qwen Pi");
    }
    return raw;
}

static int harness_is_pi(const VoiceMenuStatus *status) {
    const char *harness = status ? status->harness : NULL;
    return harness && (strcmp(harness, "pi") == 0 || strcmp(harness, "qwen-pi") == 0);
}

static void capitalize_word(const char *in, char *out, size_t cap) {
    gunichar cp = 0;
    size_t len = 0;
    size_t used = 0;
    int first = 1;
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in) return;
    while (*in && used + 1 < cap) {
        char encoded[8];
        int n;
        if (!utf8_next(in, &cp, &len)) break;
        if (first) cp = g_unichar_toupper(cp);
        first = 0;
        n = g_unichar_to_utf8(cp, encoded);
        if (n < 0 || used + (size_t)n + 1 > cap) break;
        memcpy(out + used, encoded, (size_t)n);
        used += (size_t)n;
        out[used] = 0;
        in += len;
    }
}

VoiceMenuStatus *voice_menu_status_new(void) {
    VoiceMenuStatus *status = calloc(1, sizeof *status);
    if (!status) return NULL;
    status->selection_explicit_missing = 1;
    status->auto_missing = 1;
    status->speech_available_missing = 1;
    status->can_speak_missing = 1;
    return status;
}

static void free_named(VoiceNamed *items, size_t count) {
    size_t i;
    if (!items) return;
    for (i = 0; i < count; i++) {
        free(items[i].key);
        free(items[i].label);
    }
    free(items);
}

void voice_menu_status_free(VoiceMenuStatus *status) {
    size_t i;
    if (!status) return;
    free(status->phase);
    free(status->pane);
    free(status->agent_state);
    free(status->connection_state);
    free(status->retained_source);
    free(status->harness);
    free(status->error);
    free(status->recording_label);
    free(status->session_label);
    free(status->selected_voice);
    free(status->selected_stt);
    free(status->models);
    free(status->model_error);
    free(status->speech_backend);
    free(status->microphone.name);
    free(status->microphone.error);
    for (i = 0; i < status->session_count; i++) {
        free(status->sessions[i].token);
        free(status->sessions[i].id);
        free(status->sessions[i].label);
        free(status->sessions[i].full_label);
        free(status->sessions[i].thinking);
        free(status->sessions[i].connection_state);
    }
    free(status->sessions);
    free_named(status->voices, status->voice_count);
    free_named(status->speech_backends, status->speech_count);
    free_named(status->stt_backends, status->stt_count);
    free(status);
}

int voice_menu_add_named(VoiceNamed **items, size_t *count, const char *key, const char *label) {
    VoiceNamed *next;
    if (!items || !count || !key) return -1;
    next = realloc(*items, (*count + 1) * sizeof *next);
    if (!next) return -1;
    *items = next;
    next[*count].key = xstrdup(key);
    next[*count].label = xstrdup(label ? label : "");
    if (!next[*count].key || !next[*count].label) return -1;
    (*count)++;
    return 0;
}

int voice_menu_add_session(VoiceMenuStatus *status, const char *token, const char *id,
    const char *label, const char *full_label, const char *thinking,
    const char *connection_state, int selected, int team_child) {
    VoiceSessionRow *next;
    if (!status) return -1;
    next = realloc(status->sessions, (status->session_count + 1) * sizeof *next);
    if (!next) return -1;
    status->sessions = next;
    memset(&next[status->session_count], 0, sizeof *next);
    next[status->session_count].token = xstrdup(token);
    next[status->session_count].id = xstrdup(id);
    next[status->session_count].label = xstrdup(label);
    next[status->session_count].full_label = xstrdup(full_label);
    next[status->session_count].thinking = xstrdup(thinking);
    next[status->session_count].connection_state = xstrdup(connection_state);
    next[status->session_count].selected = selected;
    next[status->session_count].team_child = team_child;
    status->session_count++;
    return 0;
}

static int append_contains(const char *text, const char *needle) {
    return text && needle && strstr(text, needle) != NULL;
}

int voice_menu_status_contains(const VoiceMenuStatus *status, const char *needle) {
    size_t i;
    if (!status || !needle) return 0;
    if (append_contains(status->phase, needle) || append_contains(status->pane, needle)
        || append_contains(status->agent_state, needle)
        || append_contains(status->connection_state, needle)
        || append_contains(status->retained_source, needle)
        || append_contains(status->harness, needle)
        || append_contains(status->error, needle)
        || append_contains(status->recording_label, needle)
        || append_contains(status->session_label, needle)
        || append_contains(status->selected_voice, needle)
        || append_contains(status->selected_stt, needle)
        || append_contains(status->models, needle)
        || append_contains(status->model_error, needle)
        || append_contains(status->speech_backend, needle)
        || append_contains(status->microphone.name, needle)
        || append_contains(status->microphone.error, needle)) return 1;
    for (i = 0; i < status->session_count; i++) {
        if (append_contains(status->sessions[i].token, needle)
            || append_contains(status->sessions[i].id, needle)
            || append_contains(status->sessions[i].label, needle)
            || append_contains(status->sessions[i].full_label, needle)
            || append_contains(status->sessions[i].thinking, needle)) return 1;
    }
    for (i = 0; i < status->voice_count; i++) {
        if (append_contains(status->voices[i].key, needle)
            || append_contains(status->voices[i].label, needle)) return 1;
    }
    for (i = 0; i < status->speech_count; i++) {
        if (append_contains(status->speech_backends[i].key, needle)
            || append_contains(status->speech_backends[i].label, needle)) return 1;
    }
    for (i = 0; i < status->stt_count; i++) {
        if (append_contains(status->stt_backends[i].key, needle)
            || append_contains(status->stt_backends[i].label, needle)) return 1;
    }
    return 0;
}

static const char *json_str(yyjson_val *value) {
    return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static int json_truthy(yyjson_val *value) {
    if (!value || yyjson_is_null(value) || yyjson_is_false(value)) return 0;
    if (yyjson_is_true(value)) return 1;
    if (yyjson_is_str(value)) return json_str(value)[0] != 0;
    if (yyjson_is_num(value)) return yyjson_get_num(value) != 0.0;
    if (yyjson_is_arr(value)) return yyjson_arr_size(value) > 0;
    if (yyjson_is_obj(value)) return yyjson_obj_size(value) > 0;
    return 0;
}

static void tri_bool(yyjson_val *root, const char *key, int *missing, int *value) {
    yyjson_val *item = yyjson_obj_get(root, key);
    if (!item) {
        *missing = 1;
        *value = 0;
        return;
    }
    *missing = 0;
    *value = json_truthy(item);
}

static int add_named_obj(VoiceNamed **items, size_t *count, yyjson_val *obj) {
    yyjson_val *key;
    yyjson_val *val;
    yyjson_obj_iter iter;
    if (!yyjson_is_obj(obj)) return 0;
    yyjson_obj_iter_init(obj, &iter);
    while ((key = yyjson_obj_iter_next(&iter))) {
        const char *name = json_str(key);
        val = yyjson_obj_iter_get_val(key);
        if (!name) continue;
        if (voice_menu_add_named(items, count, name, json_str(val) ? json_str(val) : "")) return -1;
    }
    return 0;
}

int voice_menu_status_from_json(const char *json, VoiceMenuStatus **out, char **error) {
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *sessions;
    yyjson_val *mic;
    VoiceMenuStatus *status;
    size_t idx, max;
    yyjson_val *row;
    if (!out) return fail(error, "Voice status needs an output");
    *out = NULL;
    if (!json) return fail(error, "Voice status was empty");
    doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return fail(error, "Voice status was not valid JSON");
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return fail(error, "Voice status was not an object");
    }
    status = voice_menu_status_new();
    if (!status) {
        yyjson_doc_free(doc);
        return fail(error, "Out of memory");
    }
    status->phase = xstrdup(json_str(yyjson_obj_get(root, "phase")));
    if (yyjson_obj_get(root, "phase") && !status->phase) status->phase = xstrdup("");
    status->preparing = json_truthy(yyjson_obj_get(root, "preparing_transcription"));
    status->pane = xstrdup(json_str(yyjson_obj_get(root, "pane")));
    status->draft = json_truthy(yyjson_obj_get(root, "draft"));
    status->pending = json_truthy(yyjson_obj_get(root, "pending"));
    status->retry = json_truthy(yyjson_obj_get(root, "retry"));
    status->speaking = json_truthy(yyjson_obj_get(root, "speaking"));
    status->responding = json_truthy(yyjson_obj_get(root, "responding"));
    status->agent_state = xstrdup(json_str(yyjson_obj_get(root, "agent_state")));
    status->connection_state = xstrdup(json_str(yyjson_obj_get(root, "connection_state")));
    status->retained = json_truthy(yyjson_obj_get(root, "retained"));
    status->retained_source = xstrdup(json_str(yyjson_obj_get(root, "retained_source")));
    status->harness = xstrdup(json_str(yyjson_obj_get(root, "harness")));
    if (yyjson_is_num(yyjson_obj_get(root, "recording_seconds"))) {
        status->has_recording_seconds = 1;
        status->recording_seconds = yyjson_get_num(yyjson_obj_get(root, "recording_seconds"));
    }
    if (yyjson_is_num(yyjson_obj_get(root, "input_level"))) {
        status->has_input_level = 1;
        status->input_level = yyjson_get_num(yyjson_obj_get(root, "input_level"));
    }
    status->error = xstrdup(json_str(yyjson_obj_get(root, "error")));
    status->recording_label = xstrdup(json_str(yyjson_obj_get(root, "recording_label")));
    status->session_label = xstrdup(json_str(yyjson_obj_get(root, "session_label")));
    tri_bool(root, "selection_explicit", &status->selection_explicit_missing, &status->selection_explicit);
    status->show_team = json_truthy(yyjson_obj_get(root, "show_team"));
    tri_bool(root, "auto", &status->auto_missing, &status->auto_value);
    status->selected_voice = xstrdup(json_str(yyjson_obj_get(root, "selected_voice")));
    status->selected_stt = xstrdup(json_str(yyjson_obj_get(root, "selected_stt")));
    tri_bool(root, "speech_available", &status->speech_available_missing, &status->speech_available);
    tri_bool(root, "can_speak", &status->can_speak_missing, &status->can_speak);
    status->models = xstrdup(json_str(yyjson_obj_get(root, "models")));
    status->model_error = xstrdup(json_str(yyjson_obj_get(root, "model_error")));
    status->rebind_needed = json_truthy(yyjson_obj_get(root, "rebind_needed"));
    status->has_reply = json_truthy(yyjson_obj_get(root, "reply"));
    status->speech_backend = xstrdup(json_str(yyjson_obj_get(root, "speech_backend")));
    status->conversation_available = json_truthy(yyjson_obj_get(root, "conversation_available"));
    if (add_named_obj(&status->voices, &status->voice_count, yyjson_obj_get(root, "voices"))
        || add_named_obj(&status->speech_backends, &status->speech_count, yyjson_obj_get(root, "speech_backends"))
        || add_named_obj(&status->stt_backends, &status->stt_count, yyjson_obj_get(root, "stt_backends"))) {
        voice_menu_status_free(status);
        yyjson_doc_free(doc);
        return fail(error, "Out of memory");
    }
    mic = yyjson_obj_get(root, "microphone");
    if (yyjson_is_obj(mic) && yyjson_obj_size(mic) > 0) {
        status->microphone.present = 1;
        status->microphone.name = xstrdup(json_str(yyjson_obj_get(mic, "name")));
        status->microphone.muted = json_truthy(yyjson_obj_get(mic, "muted"));
        status->microphone.clipping = json_truthy(yyjson_obj_get(mic, "clipping"));
        status->microphone.missing = json_truthy(yyjson_obj_get(mic, "missing"));
        status->microphone.error = xstrdup(json_str(yyjson_obj_get(mic, "error")));
    }
    sessions = yyjson_obj_get(root, "sessions");
    if (yyjson_is_arr(sessions)) {
        yyjson_arr_foreach(sessions, idx, max, row) {
            const char *token = json_str(yyjson_obj_get(row, "token"));
            const char *id = json_str(yyjson_obj_get(row, "id"));
            if (!yyjson_is_obj(row)) continue;
            if (voice_menu_add_session(status, token, id,
                json_str(yyjson_obj_get(row, "label")),
                json_str(yyjson_obj_get(row, "full_label")),
                json_str(yyjson_obj_get(row, "thinking")),
                json_str(yyjson_obj_get(row, "connection_state")),
                json_truthy(yyjson_obj_get(row, "selected")),
                json_truthy(yyjson_obj_get(row, "team_child")))) {
                voice_menu_status_free(status);
                yyjson_doc_free(doc);
                return fail(error, "Out of memory");
            }
        }
    }
    yyjson_doc_free(doc);
    *out = status;
    (void)error;
    return 0;
}

static int add_action(VoicePresentation *view, const char *action, const char *label, int enabled) {
    VoiceAction *next = realloc(view->actions, (view->action_count + 1) * sizeof *next);
    if (!next) return -1;
    view->actions = next;
    next[view->action_count].action = xstrdup(action);
    next[view->action_count].label = xstrdup(label ? label : "");
    next[view->action_count].enabled = enabled;
    if (!next[view->action_count].action || !next[view->action_count].label) return -1;
    view->action_count++;
    return 0;
}

static int add_context(VoicePresentation *view, const char *line) {
    char **next;
    if (!line) return -1;
    next = realloc(view->context, (view->context_count + 1) * sizeof *next);
    if (!next) return -1;
    view->context = next;
    next[view->context_count] = xstrdup(line);
    if (!next[view->context_count]) return -1;
    view->context_count++;
    return 0;
}

static int add_identity(VoicePresentation *view, const char *action, const char *token, const char *id) {
    VoiceIdentity *next = realloc(view->identities, (view->identity_count + 1) * sizeof *next);
    if (!next) return -1;
    view->identities = next;
    next[view->identity_count].action = xstrdup(action);
    next[view->identity_count].token = xstrdup(token ? token : "");
    next[view->identity_count].id = xstrdup(id ? id : "");
    if (!next[view->identity_count].action || !next[view->identity_count].token
        || !next[view->identity_count].id) return -1;
    view->identity_count++;
    return 0;
}

static int add_full(VoicePresentation *view, const char *action, const char *label) {
    VoiceFullLabel *next = realloc(view->full_labels, (view->full_label_count + 1) * sizeof *next);
    if (!next) return -1;
    view->full_labels = next;
    next[view->full_label_count].action = xstrdup(action);
    next[view->full_label_count].label = xstrdup(label ? label : "");
    if (!next[view->full_label_count].action || !next[view->full_label_count].label) return -1;
    view->full_label_count++;
    return 0;
}

static int session_visible(const VoiceSessionRow *row, int show_team) {
    return row->selected || show_team || !row->team_child;
}

static const VoiceSessionRow *selected_row(const VoiceMenuStatus *status) {
    size_t i;
    if (!status) return NULL;
    for (i = 0; i < status->session_count; i++) {
        if (status->sessions[i].selected) return &status->sessions[i];
    }
    return NULL;
}

int voice_selected_same(const VoiceMenuStatus *left, const VoiceMenuStatus *right) {
    size_t i, j;
    size_t ln = 0, rn = 0;
    if (!left || !right) return left == right ? 0 : 1;
    for (i = 0; i < left->session_count; i++) if (left->sessions[i].selected) ln++;
    for (i = 0; i < right->session_count; i++) if (right->sessions[i].selected) rn++;
    if (ln != rn) return 1;
    j = 0;
    for (i = 0; i < left->session_count; i++) {
        const VoiceSessionRow *a;
        const VoiceSessionRow *b = NULL;
        if (!left->sessions[i].selected) continue;
        a = &left->sessions[i];
        for (; j < right->session_count; j++) {
            if (right->sessions[j].selected) {
                b = &right->sessions[j];
                j++;
                break;
            }
        }
        if (!b) return 1;
        if (strcmp(a->token ? a->token : "", b->token ? b->token : "") != 0) return 1;
        if (strcmp(a->id ? a->id : "", b->id ? b->id : "") != 0) return 1;
    }
    return 0;
}

const VoiceAction *voice_presentation_action(const VoicePresentation *view, const char *action) {
    size_t i;
    if (!view || !action) return NULL;
    for (i = 0; i < view->action_count; i++) {
        if (strcmp(view->actions[i].action, action) == 0) return &view->actions[i];
    }
    return NULL;
}

int voice_presentation_contains(const VoicePresentation *view, const char *needle) {
    size_t i;
    if (!view || !needle) return 0;
    if (append_contains(view->label, needle) || append_contains(view->selected_voice, needle)
        || append_contains(view->selected_stt, needle)
        || append_contains(view->selected_session, needle)) return 1;
    for (i = 0; i < view->context_count; i++) {
        if (append_contains(view->context[i], needle)) return 1;
    }
    for (i = 0; i < view->action_count; i++) {
        if (append_contains(view->actions[i].action, needle)
            || append_contains(view->actions[i].label, needle)) return 1;
    }
    for (i = 0; i < view->full_label_count; i++) {
        if (append_contains(view->full_labels[i].label, needle)) return 1;
    }
    return 0;
}

void voice_presentation_free(VoicePresentation *view) {
    size_t i;
    if (!view) return;
    free(view->label);
    free(view->colour);
    free(view->glyph);
    free(view->selected_voice);
    free(view->selected_stt);
    free(view->selected_session);
    for (i = 0; i < view->context_count; i++) free(view->context[i]);
    free(view->context);
    for (i = 0; i < view->action_count; i++) {
        free(view->actions[i].action);
        free(view->actions[i].label);
    }
    free(view->actions);
    for (i = 0; i < view->identity_count; i++) {
        free(view->identities[i].action);
        free(view->identities[i].token);
        free(view->identities[i].id);
    }
    free(view->identities);
    for (i = 0; i < view->full_label_count; i++) {
        free(view->full_labels[i].action);
        free(view->full_labels[i].label);
    }
    free(view->full_labels);
    free(view);
}

int voice_presentation(const VoiceMenuStatus *status, VoicePresentation **out, char **error) {
    VoiceMenuStatus empty;
    VoicePresentation *view;
    const char *phase;
    int busy, selected, ready, pending, speaking, responding, blocked, connected, retained, usable;
    char *harness;
    const char *glyph = "microphone";
    const char *colour = "grey";
    char label[1024];
    const VoiceSessionRow *row;
    char *destination = NULL;
    size_t i;
    if (!out) return fail(error, "Presentation needs an output");
    *out = NULL;
    if (!status) {
        memset(&empty, 0, sizeof empty);
        empty.selection_explicit_missing = 1;
        empty.speech_available_missing = 1;
        empty.can_speak_missing = 1;
        empty.auto_missing = 1;
        status = &empty;
    }
    view = calloc(1, sizeof *view);
    if (!view) return fail(error, "Out of memory");
    phase = phase_of(status);
    busy = voice_menu_busy(status);
    selected = pane_selected(status);
    ready = status->draft || status->pending;
    pending = status->pending;
    speaking = status->speaking;
    responding = status->responding || (status->agent_state && strcmp(status->agent_state, "working") == 0);
    blocked = status->agent_state && strcmp(status->agent_state, "blocked") == 0;
    connected = selected && strcmp(connection_of(status), "ready") == 0;
    retained = status->retained;
    usable = connected && !blocked && !retained;
    harness = harness_label(status);
    if (!harness) {
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (strcmp(phase, "idle") == 0) {
        snprintf(label, sizeof label, "%s", selected ? "Ready to record" : "No voice session selected");
        colour = "grey";
    } else if (strcmp(phase, "starting") == 0) {
        snprintf(label, sizeof label, "Starting microphone");
        colour = "amber";
    } else if (strcmp(phase, "recording") == 0) {
        snprintf(label, sizeof label, "Recording");
        colour = "red";
    } else if (strcmp(phase, "stopping") == 0) {
        snprintf(label, sizeof label, "Finishing recording");
        colour = "amber";
    } else if (strcmp(phase, "transcribing") == 0) {
        snprintf(label, sizeof label, "Transcribing");
        colour = "amber";
    } else if (strcmp(phase, "draft") == 0) {
        snprintf(label, sizeof label, "Dictation ready to send");
        colour = "green";
    } else if (strcmp(phase, "error") == 0) {
        snprintf(label, sizeof label, "Voice error; try recording again");
        colour = "orange";
    } else {
        snprintf(label, sizeof label, "Voice unavailable");
        colour = "orange";
    }
    if (strcmp(phase, "recording") == 0) {
        double seconds = status->has_recording_seconds ? status->recording_seconds : 0;
        double level = status->has_input_level ? status->input_level : 0;
        int elapsed = (int)trunc(seconds);
        int percent;
        char extra[64];
        if (!isfinite(seconds) || elapsed < 0) elapsed = 0;
        if (!isfinite(level)) level = 0;
        percent = (int)trunc(level * 100.0);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        snprintf(extra, sizeof extra, " %02d:%02d (input %d%%)", elapsed / 60, elapsed % 60, percent);
        strncat(label, extra, sizeof label - strlen(label) - 1);
    } else if (strcmp(phase, "error") == 0 && status->error && status->error[0]) {
        char *detail = public_dup(status->error, 120);
        if (!detail) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        snprintf(label, sizeof label, "Voice error: %s", detail);
        free(detail);
    }
    if (strcmp(phase, "error") == 0) glyph = "blocked";
    else if (!busy) {
        if (ready) {
            if (pending) snprintf(label, sizeof label, "Dictation waiting for %s", harness);
            else snprintf(label, sizeof label, "Dictation ready; Super+Space sends it");
            colour = "green";
            glyph = "check";
        } else if (speaking) {
            snprintf(label, sizeof label, "Reading reply aloud");
            colour = "blue";
            glyph = "speaker";
        } else if (selected && blocked) {
            snprintf(label, sizeof label, "%s needs your attention", harness);
            colour = "orange";
            glyph = "blocked";
        } else if (selected && responding) {
            snprintf(label, sizeof label, "%s is responding", harness);
            colour = "blue";
            glyph = "dots";
        }
    }
    if (status->preparing) {
        snprintf(label, sizeof label, "Preparing transcription");
        colour = "amber";
    } else if (!busy && !speaking && strcmp(phase, "error") != 0
        && (strcmp(connection_of(status), "connecting") == 0
            || strcmp(connection_of(status), "reconnecting") == 0)) {
        capitalize_word(connection_of(status), label, sizeof label);
        colour = "amber";
    } else if (!selected && !busy && !speaking && strcmp(phase, "error") != 0) {
        snprintf(label, sizeof label, "No voice session selected");
    }
    row = selected_row(status);
    if (busy && status->recording_label && status->recording_label[0]) destination = public_dup(status->recording_label, VOICE_LABEL_UNLIMITED);
    else if (row && row->full_label && row->full_label[0]) destination = public_dup(row->full_label, VOICE_LABEL_UNLIMITED);
    else if (status->session_label && status->session_label[0]) destination = public_dup(status->session_label, VOICE_LABEL_UNLIMITED);
    if (selected || (busy && status->recording_label && status->recording_label[0])
        || strcmp(connection_of(status), "reconnecting") == 0) {
        char *session = public_dup(destination ? destination : (status->pane && status->pane[0] ? status->pane : "Previous destination"), VOICE_LABEL_UNLIMITED);
        char line[2048];
        if (!session) {
            free(destination);
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        snprintf(line, sizeof line, "%s: %s", harness, session);
        free(session);
        if (add_context(view, line)) {
            free(destination);
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        if (strcmp(connection_of(status), "ready") != 0) {
            char conn[256];
            capitalize_word(connection_of(status), conn, sizeof conn);
            if (add_context(view, conn)) {
                free(destination);
                free(harness);
                voice_presentation_free(view);
                return fail(error, "Out of memory");
            }
        }
        if (responding && strcmp(glyph, "dots") != 0) {
            snprintf(line, sizeof line, "%s is responding", harness);
            if (add_context(view, line)) {
                free(destination);
                free(harness);
                voice_presentation_free(view);
                return fail(error, "Out of memory");
            }
        } else if (blocked) {
            snprintf(line, sizeof line, "%s needs your attention", harness);
            if (strcmp(label, line) != 0 && add_context(view, line)) {
                free(destination);
                free(harness);
                voice_presentation_free(view);
                return fail(error, "Out of memory");
            }
        }
    }
    free(destination);
    if (status->microphone.present) {
        char *name = public_dup(or_text(status->microphone.name, "Unknown"), 120);
        char line[1024];
        if (!name) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        snprintf(line, sizeof line, "Microphone: %s", name);
        free(name);
        if (status->microphone.muted) strncat(line, " (muted)", sizeof line - strlen(line) - 1);
        if (status->microphone.clipping) strncat(line, " (clipping; lower input volume)", sizeof line - strlen(line) - 1);
        if (status->microphone.missing) strncat(line, " (preferred microphone unavailable; using default)", sizeof line - strlen(line) - 1);
        if (add_context(view, line)) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        if (status->microphone.error && status->microphone.error[0]) {
            char *mic_error = public_dup(status->microphone.error, 120);
            if (!mic_error || add_context(view, mic_error)) {
                free(mic_error);
                free(harness);
                voice_presentation_free(view);
                return fail(error, "Out of memory");
            }
            free(mic_error);
        }
    }
    if (status->models && strcmp(status->models, "loading") == 0) {
        if (add_context(view, "Speech models loading")) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        if (strcmp(colour, "grey") == 0) colour = "amber";
    } else if (status->models && strcmp(status->models, "unavailable") == 0) {
        if (add_context(view, "Speech models unavailable")) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        if (status->model_error && status->model_error[0]) {
            char *model_error = public_dup(status->model_error, 120);
            if (!model_error || add_context(view, model_error)) {
                free(model_error);
                free(harness);
                voice_presentation_free(view);
                return fail(error, "Out of memory");
            }
            free(model_error);
        }
        if ((strcmp(colour, "grey") == 0 || strcmp(colour, "amber") == 0) && !busy) colour = "orange";
    }
    if (status->rebind_needed) {
        if (add_context(view, "Conversation changed; choose Bind to current conversation")) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        if (strcmp(phase, "idle") == 0 && strcmp(colour, "grey") == 0) colour = "amber";
    }
    view->label = xstrdup(label);
    view->colour = xstrdup(colour);
    view->glyph = xstrdup(glyph);
    view->auto_on = auto_on(status);
    view->selected_voice = xstrdup(or_text(status->selected_voice, "none"));
    view->selected_stt = xstrdup(or_text(status->selected_stt, "none"));
    view->show_team = status->show_team;
    if (!view->label || !view->colour || !view->glyph || !view->selected_voice || !view->selected_stt) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (add_action(view, "team-toggle", "Show team members", 1)) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (speech_available(status) && add_action(view, "auto-toggle", "Read replies aloud", 1)) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (selected && !can_speak(status) && add_context(view, "Team members are silent")) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    for (i = 0; i < status->voice_count; i++) {
        char action[256];
        char *voice_label = public_dup(status->voices[i].label, 120);
        snprintf(action, sizeof action, "voice:%s", status->voices[i].key);
        if (!voice_label || add_action(view, action, voice_label, 1)) {
            free(voice_label);
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        free(voice_label);
    }
    if (status->speech_count > 1) {
        for (i = 0; i < status->speech_count; i++) {
            char action[256];
            char *speech_label = public_dup(status->speech_backends[i].label, 120);
            snprintf(action, sizeof action, "speech:%s", status->speech_backends[i].key);
            if (!speech_label || add_action(view, action, speech_label, !busy)) {
                free(speech_label);
                free(harness);
                voice_presentation_free(view);
                return fail(error, "Out of memory");
            }
            free(speech_label);
        }
    }
    for (i = 0; i < status->stt_count; i++) {
        char action[256];
        char *stt_label = public_dup(status->stt_backends[i].label, 120);
        snprintf(action, sizeof action, "stt:%s", status->stt_backends[i].key);
        if (!stt_label || add_action(view, action, stt_label, !busy)) {
            free(stt_label);
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        free(stt_label);
    }
    const char *selected_stt_label = view->selected_stt;
    for (size_t i = 0; i < status->stt_count; i++)
        if (strcmp(status->stt_backends[i].key, view->selected_stt) == 0)
            selected_stt_label = status->stt_backends[i].label;
    char dictation_context[160];
    snprintf(dictation_context, sizeof dictation_context, "Dictation: %.120s", selected_stt_label);
    if (add_context(view, dictation_context)) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (connected && !blocked && !busy && !pending && !speaking && !responding && can_speak(status) && status->has_reply
        && add_action(view, "read", "Replay last reply", 1)) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (busy || speaking || retained
        || strcmp(connection_of(status), "connecting") == 0
        || strcmp(connection_of(status), "reconnecting") == 0
        || (status->models && strcmp(status->models, "loading") == 0)) {
        const char *stop = "Stop";
        if (strcmp(phase, "transcribing") == 0 || status->preparing) stop = "Cancel transcription";
        else if (busy) stop = "Cancel recording";
        else if (speaking) stop = "Stop speaking";
        if (add_action(view, "stop", stop, 1)) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
    }
    if (usable && ready && !busy && add_action(view, "append", "Record more", 1)) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (usable && status->retry && !busy && add_action(view, "retry", "Retry transcription", 1)) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if ((pending || status->retry) && !busy) {
        const char *discard = pending ? "Discard retained dictation" : "Discard retained recording";
        if (add_action(view, "discard", discard, 1)) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
    }
    if (selected && status->rebind_needed && !busy && add_action(view, "rebind", "Bind to current conversation", 1)) {
        free(harness);
        voice_presentation_free(view);
        return fail(error, "Out of memory");
    }
    if (retained) {
        char *source = public_dup(or_text(status->retained_source, "Previous destination"), 120);
        char line[512];
        int stage = connected && harness_is_pi(status) && selection_explicit(status)
            && !busy && !blocked && !responding;
        if (!source) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        snprintf(line, sizeof line, "Retained dictation from %s", source);
        free(source);
        if (add_context(view, line)
            || add_action(view, "recover-copy", "Copy retained dictation", 1)
            || add_action(view, "recover-stage", "Stage in selected Pi session", stage)
            || add_action(view, "recover-discard", "Discard retained dictation", 1)) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
    }
    for (i = 0; i < status->session_count; i++) {
        const VoiceSessionRow *session = &status->sessions[i];
        char action[768];
        char shown[1024];
        char *session_label;
        int confirm;
        int enabled;
        const char *session_connection;
        if (!session_visible(session, view->show_team)) continue;
        if (!session->token || !session->token[0]) continue;
        snprintf(action, sizeof action, "select:%s", session->token);
        if (session->selected) {
            free(view->selected_session);
            view->selected_session = xstrdup(action);
            if (!view->selected_session) {
                free(harness);
                voice_presentation_free(view);
                return fail(error, "Out of memory");
            }
        }
        if (add_identity(view, action, session->token, session->id)) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        session_label = public_dup(or_text(session->full_label, or_text(session->label, session->token)), VOICE_LABEL_UNLIMITED);
        if (!session_label || add_full(view, action, session_label)) {
            free(session_label);
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        free(session_label);
        confirm = session->selected && retained && harness_is_pi(status) && !selection_explicit(status);
        session_label = public_dup(or_text(session->label, session->token), 120);
        if (!session_label) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
        if (confirm) snprintf(shown, sizeof shown, "Confirm %s for retained dictation", session_label);
        else snprintf(shown, sizeof shown, "%s", session_label);
        free(session_label);
        session_connection = session->connection_state ? session->connection_state : "ready";
        enabled = !busy && (!session->selected || confirm) && strcmp(session_connection, "ready") == 0;
        if (add_action(view, action, shown, enabled)) {
            free(harness);
            voice_presentation_free(view);
            return fail(error, "Out of memory");
        }
    }
    free(harness);
    *out = view;
    return 0;
}
