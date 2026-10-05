#define _POSIX_C_SOURCE 200809L
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "spoken.h"
#include "text.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int copy_cap(char *dst, size_t cap, const char *src) {
    if (!src) {
        if (cap) dst[0] = 0;
        return 0;
    }
    if (strlen(src) >= cap) return -1;
    memcpy(dst, src, strlen(src) + 1);
    return 0;
}

static int bad_identity(const char *value) {
    return !value || !value[0] || strlen(value) > 4096 || has_control_characters(value);
}

static int cmdline_is_pi(int pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0) return 0;
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] != 0) continue;
        const char *arg = buf;
        for (ssize_t j = 0; j < i; j++) {
            if (buf[j] == 0) arg = buf + j + 1;
        }
        size_t len = strlen(arg);
        if (len >= 3 && strcmp(arg + len - 3, "/pi") == 0) return 1;
        if (len >= 14 && strcmp(arg + len - 14, "/pi/dist/cli.js") == 0) return 1;
        if (len >= 28 && strcmp(arg + len - 28, "/pi-coding-agent/dist/cli.js") == 0) return 1;
    }
    return 0;
}

static int name_is_pi(int pid, const char *name) {
    char lower[128];
    size_t n = 0;
    if (!name) return 0;
    for (; name[n] && n + 1 < sizeof lower; n++) lower[n] = (char)tolower((unsigned char)name[n]);
    lower[n] = 0;
    if (strcmp(lower, "node") == 0 || strcmp(lower, "nodejs") == 0) return cmdline_is_pi(pid);
    return strcmp(lower, "pi") == 0 || strcmp(lower, "qwen-pi") == 0;
}

static int herdr_result(voice_controller *app, const target_data *target, const char *const *argv, yyjson_doc **doc, char *err, size_t cap) {
    if (!app->deps.herdr.request) {
        snprintf(err, cap, "Could not reach the selected Herdr pane");
        return -1;
    }
    controller_target_view view;
    controller_view(target, &view);
    char *json = NULL;
    int rc = app->deps.herdr.request(app->deps.herdr.user, &view, argv, &json, err, cap);
    if (rc != 0) return -1;
    *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    free(json);
    if (!*doc || !yyjson_is_obj(yyjson_doc_get_root(*doc))) {
        yyjson_doc_free(*doc);
        *doc = NULL;
        snprintf(err, cap, "Invalid response from the Herdr pane");
        return -1;
    }
    return 0;
}

int controller_validate_attachment(voice_controller *app, const target_data *incoming, target_data *out, char *err, size_t cap) {
    memset(out, 0, sizeof *out);
    if (!incoming || (strcmp(incoming->harness, "pi") != 0 && strcmp(incoming->harness, "qwen-pi") != 0)) {
        snprintf(err, cap, "Only Pi sessions support managed attachment");
        return -1;
    }
    if (bad_identity(incoming->pane) || bad_identity(incoming->socket) || bad_identity(incoming->session)
        || bad_identity(incoming->bridge_id) || bad_identity(incoming->adapter_socket)) {
        snprintf(err, cap, "Invalid Pi attachment identity");
        return -1;
    }
    if (incoming->pid <= 0 || !incoming->has_activation || incoming->activation <= 0 || incoming->activation > ((int64_t)1 << 53) - 1
        || (incoming->has_team_child && incoming->team_child != 0 && incoming->team_child != 1)) {
        snprintf(err, cap, "Invalid Pi process or activation");
        return -1;
    }
    char start[CTRL_START];
    if (process_start_ticks(incoming->pid, start, sizeof start) != 0 || !process_uid_matches(incoming->pid)) {
        snprintf(err, cap, "Pi process is unavailable");
        return -1;
    }
    uint64_t adev = 0, aino = 0;
    if (validate_endpoint(app->runtime, incoming->adapter_socket, incoming->pid, incoming->bridge_id, &adev, &aino) != 0) {
        snprintf(err, cap, "Pi endpoint is unavailable or invalid");
        return -1;
    }
    char canonical[CTRL_PATH];
    uint64_t sdev = 0, sino = 0;
    if (socket_instance(incoming->socket, canonical, sizeof canonical, &sdev, &sino) != 0) {
        snprintf(err, cap, "Invalid Pi attachment identity");
        return -1;
    }
    *out = *incoming;
    copy_cap(out->socket, sizeof out->socket, canonical);
    out->has_socket_instance = 1;
    out->socket_device = sdev;
    out->socket_inode = sino;
    out->has_start = 1;
    copy_cap(out->start, sizeof out->start, start);
    out->managed = 1;
    out->has_adapter = 1;
    out->adapter_device = adev;
    out->adapter_inode = aino;
    out->has_team_child = 1;
    const char *argv[] = {"pane", "process-info", "--pane", out->pane, NULL};
    yyjson_doc *doc = NULL;
    if (herdr_result(app, out, argv, &doc, err, cap) != 0) return -1;
    yyjson_val *info = yyjson_obj_get(yyjson_doc_get_root(doc), "process_info");
    if (!yyjson_is_obj(info) || !yyjson_is_str(yyjson_obj_get(info, "pane_id"))
        || strcmp(yyjson_get_str(yyjson_obj_get(info, "pane_id")), out->pane) != 0) {
        yyjson_doc_free(doc);
        snprintf(err, cap, "Herdr did not confirm the full Pi pane identity");
        return -1;
    }
    yyjson_val *fg = yyjson_obj_get(info, "foreground_processes");
    if (!yyjson_is_arr(fg)) {
        yyjson_doc_free(doc);
        snprintf(err, cap, "Invalid Pi foreground process information");
        return -1;
    }
    const char *name = NULL;
    size_t idx, max;
    yyjson_val *row;
    yyjson_arr_foreach(fg, idx, max, row) {
        if (!yyjson_is_obj(row)) {
            yyjson_doc_free(doc);
            snprintf(err, cap, "Invalid Pi foreground process information");
            return -1;
        }
        yyjson_val *pid = yyjson_obj_get(row, "pid");
        if (yyjson_is_int(pid) && yyjson_get_int(pid) == out->pid)
            name = yyjson_is_str(yyjson_obj_get(row, "name")) ? yyjson_get_str(yyjson_obj_get(row, "name")) : "";
    }
    int is_pi = name_is_pi(out->pid, name);
    yyjson_doc_free(doc);
    if (!is_pi) {
        snprintf(err, cap, "Pi process is no longer in the claimed foreground pane");
        return -1;
    }
    char start2[CTRL_START];
    uint64_t sdev2 = 0, sino2 = 0, adev2 = 0, aino2 = 0;
    char canonical2[CTRL_PATH];
    if (process_start_ticks(out->pid, start2, sizeof start2) != 0 || strcmp(start2, start) != 0
        || socket_instance(out->socket, canonical2, sizeof canonical2, &sdev2, &sino2) != 0
        || sdev2 != sdev || sino2 != sino
        || validate_endpoint(app->runtime, out->adapter_socket, out->pid, out->bridge_id, &adev2, &aino2) != 0
        || adev2 != adev || aino2 != aino) {
        snprintf(err, cap, "Pi identity changed during attachment");
        return -1;
    }
    return 0;
}

static AttachmentTarget as_attachment(const target_data *data) {
    AttachmentTarget target = {0};
    target.socket = data->socket;
    target.socket_device = data->socket_device;
    target.socket_inode = data->socket_inode;
    target.pane = data->pane;
    target.pid = data->pid;
    target.start = data->start;
    target.bridge_id = data->bridge_id;
    return target;
}

static session_entry *new_managed(voice_controller *app, const char *token, const target_data *data) {
    session_entry *slot = session_slot_pub(app);
    if (!slot) return NULL;
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    snprintf(slot->token, sizeof slot->token, "%s", token);
    target_data owned = *data;
    snprintf(owned.token, sizeof owned.token, "%s", token);
    slot->target = controller_snap(&owned);
    if (data->has_session && !data->session_null) {
        slot->has_thread = 1;
        slot->has_candidate = 1;
        snprintf(slot->thread, sizeof slot->thread, "%s", data->session);
        snprintf(slot->candidate, sizeof slot->candidate, "%s", data->session);
    }
    snprintf(slot->connection_state, sizeof slot->connection_state, "connecting");
    slot->has_connection = 1;
    slot->heartbeat_at = controller_now(app);
    snprintf(slot->agent_state, sizeof slot->agent_state, "unknown");
    return slot;
}

session_entry *session_slot_pub(voice_controller *app) {
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) if (!app->sessions[i].used) return &app->sessions[i];
    return NULL;
}

char *controller_attach_json(voice_controller *app, yyjson_val *incoming) {
    pthread_mutex_lock(&app->state);
    app->pending_admissions++;
    controller_population(app);
    pthread_mutex_unlock(&app->state);
    char *result = strdup("{\"state\":\"connecting\"}");
    target_data raw;
    char err[CTRL_MSG];
    if (!yyjson_is_obj(incoming) || controller_parse_target(incoming, &raw, err, sizeof err) != 0) goto done;
    for (int attempt = 0; attempt < 3; attempt++) {
        char resolved[CTRL_PATH];
        if (socket_instance(raw.socket, resolved, sizeof resolved, &(uint64_t){0}, &(uint64_t){0}) != 0) break;
        AttachmentTarget at = as_attachment(&raw);
        at.socket = resolved;
        PaneKey pane = pane_key(&at);
        pthread_mutex_lock(&app->state);
        int64_t revision = registry_revision(app->attachments, &pane);
        int closed = atomic_load(&app->attachments_closed);
        pthread_mutex_unlock(&app->state);
        if (closed) {
            free(result);
            result = strdup("{\"state\":\"superseded\"}");
            goto done;
        }
        target_data validated;
        if (controller_validate_attachment(app, &raw, &validated, err, sizeof err) != 0) break;
        char now_start[CTRL_START];
        if (process_start_ticks(validated.pid, now_start, sizeof now_start) != 0 || strcmp(now_start, validated.start) != 0) break;
        AttachmentTarget vat = as_attachment(&validated);
        BridgeIdentity identity = bridge_identity(&vat);
        pthread_mutex_lock(&app->state);
        if (atomic_load(&app->attachments_closed)) {
            pthread_mutex_unlock(&app->state);
            free(result);
            result = strdup("{\"state\":\"superseded\"}");
            goto done;
        }
        Admission admission;
        if (registry_admit(app->attachments, &identity, validated.activation, revision, NULL, &admission) != 0) {
            pthread_mutex_unlock(&app->state);
            break;
        }
        if (admission.state && strcmp(admission.state, "retry") == 0) {
            pthread_mutex_unlock(&app->state);
            continue;
        }
        if (admission.state && strcmp(admission.state, "superseded") == 0) {
            pthread_mutex_unlock(&app->state);
            free(result);
            result = strdup("{\"state\":\"superseded\"}");
            goto done;
        }
        if (admission.state && strcmp(admission.state, "existing") == 0) {
            session_entry *entry = controller_find(app, admission.token);
            int invalid = !entry || !controller_same_attachment(&entry->target->data, &validated);
            if (!invalid) {
                const target_data *cur = &entry->target->data;
                invalid = strcmp(cur->session, validated.session) != 0
                    || strcmp(cur->adapter_socket, validated.adapter_socket) != 0
                    || cur->adapter_device != validated.adapter_device
                    || cur->adapter_inode != validated.adapter_inode
                    || cur->activation != validated.activation
                    || strcmp(cur->harness, validated.harness) != 0
                    || cur->team_child != validated.team_child;
            }
            if (invalid) {
                pthread_mutex_unlock(&app->state);
                free(result);
                result = strdup("{\"state\":\"invalid\"}");
                goto done;
            }
            char buf[256];
            snprintf(buf, sizeof buf, "{\"token\":\"%s\",\"state\":\"%s\"}", admission.token, entry->connection_state);
            pthread_mutex_unlock(&app->state);
            free(result);
            result = strdup(buf);
            goto done;
        }
        int selected = admission.has_replaced && app->has_token && strcmp(admission.replaced, app->token) == 0
            && app->target && strcmp(app->target->data.pane, identity.pane.pane_id) == 0
            && strcmp(app->target->data.socket, identity.pane.socket_path) == 0;
        int explicit_sel = app->selection_explicit;
        if (admission.has_replaced) controller_remove_session(app, admission.replaced, 1);
        for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
            session_entry *old = &app->sessions[i];
            if (!old->used || !old->target || old->target->data.managed) continue;
            const target_data *ot = &old->target->data;
            if ((strcmp(ot->harness, "pi") == 0 || strcmp(ot->harness, "qwen-pi") == 0)
                && strcmp(ot->pane, validated.pane) == 0 && strcmp(ot->socket, validated.socket) == 0
                && ot->pid == validated.pid && ot->has_start && strcmp(ot->start, validated.start) == 0) {
                if (app->has_token && strcmp(old->token, app->token) == 0) {
                    selected = 1;
                    explicit_sel = app->selection_explicit;
                }
                controller_remove_session(app, old->token, 1);
            }
        }
        if (app->has_reconnect && strcmp(app->reconnect_path, identity.pane.socket_path) == 0
            && (app->reconnect_pane.socket_device != identity.pane.socket_device
                || app->reconnect_pane.socket_inode != identity.pane.socket_inode)) {
            app->has_reconnect = 0;
            app->reconnect_explicit = 0;
        }
        if (!new_managed(app, admission.token, &validated)) {
            pthread_mutex_unlock(&app->state);
            break;
        }
        controller_population(app);
        if (selected) {
            controller_load_selected(app, admission.token, explicit_sel);
            app->has_reconnect = 1;
            snprintf(app->reconnect_path, sizeof app->reconnect_path, "%s", identity.pane.socket_path);
            snprintf(app->reconnect_pane_id, sizeof app->reconnect_pane_id, "%s", identity.pane.pane_id);
            app->reconnect_pane.socket_path = app->reconnect_path;
            app->reconnect_pane.pane_id = app->reconnect_pane_id;
            app->reconnect_pane.socket_device = identity.pane.socket_device;
            app->reconnect_pane.socket_inode = identity.pane.socket_inode;
            app->reconnect_explicit = explicit_sel;
        }
        controller_save_selection(app);
        char buf[256];
        snprintf(buf, sizeof buf, "{\"token\":\"%s\",\"state\":\"connecting\"}", admission.token);
        pthread_mutex_unlock(&app->state);
        free(result);
        result = strdup(buf);
        goto done;
    }
done:
    pthread_mutex_lock(&app->state);
    app->pending_admissions--;
    controller_population(app);
    pthread_mutex_unlock(&app->state);
    return result;
}

int controller_detach(voice_controller *app, const char *token, yyjson_val *identity) {
    pthread_mutex_lock(&app->state);
    session_entry *entry = controller_find(app, token);
    int accepted = 0;
    if (entry && entry->target && entry->target->data.managed) {
        EventView target = {0}, event = {0};
        target.bridge_id = entry->target->data.bridge_id;
        target.has_activation = 1;
        target.activation = entry->target->data.activation;
        target.has_pid = 1;
        target.pid = entry->target->data.pid;
        target.session = entry->target->data.session;
        target.harness = entry->target->data.harness;
        event.bridge_id = yyjson_is_str(yyjson_obj_get(identity, "bridge_id")) ? yyjson_get_str(yyjson_obj_get(identity, "bridge_id")) : NULL;
        if (yyjson_is_int(yyjson_obj_get(identity, "activation"))) {
            event.has_activation = 1;
            event.activation = yyjson_get_sint(yyjson_obj_get(identity, "activation"));
        }
        if (yyjson_is_int(yyjson_obj_get(identity, "pid"))) {
            event.has_pid = 1;
            event.pid = yyjson_get_int(yyjson_obj_get(identity, "pid"));
        }
        if (event_matches(&target, &event, 0)) {
            controller_remove_session(app, token, 1);
            controller_save_selection(app);
            accepted = 1;
        }
    }
    pthread_mutex_unlock(&app->state);
    return accepted;
}

static int event_view_from(yyjson_val *event, EventView *out) {
    memset(out, 0, sizeof *out);
    if (!yyjson_is_obj(event)) return -1;
    out->bridge_id = yyjson_is_str(yyjson_obj_get(event, "bridge_id")) ? yyjson_get_str(yyjson_obj_get(event, "bridge_id")) : NULL;
    out->session = yyjson_is_str(yyjson_obj_get(event, "session")) ? yyjson_get_str(yyjson_obj_get(event, "session")) : NULL;
    out->harness = yyjson_is_str(yyjson_obj_get(event, "harness")) ? yyjson_get_str(yyjson_obj_get(event, "harness")) : NULL;
    if (yyjson_is_int(yyjson_obj_get(event, "activation"))) {
        out->has_activation = 1;
        out->activation = yyjson_get_sint(yyjson_obj_get(event, "activation"));
    }
    if (yyjson_is_int(yyjson_obj_get(event, "pid"))) {
        out->has_pid = 1;
        out->pid = yyjson_get_int(yyjson_obj_get(event, "pid"));
    }
    return 0;
}

void controller_set_activity(session_entry *entry, const char *state) {
    snprintf(entry->agent_state, sizeof entry->agent_state, "%s", state ? state : "unknown");
    entry->activity_revision++;
}

static void settle(session_entry *entry, const char *turn) {
    if (turn && entry->has_active_turn && strcmp(turn, entry->active_turn) != 0) return;
    if (turn && entry->has_active_turn && strcmp(turn, entry->active_turn) == 0) {
        controller_set_activity(entry, "idle");
        return;
    }
    controller_set_activity(entry, entry->agent_state);
    entry->has_activity_checked = 0;
}

int controller_notify(voice_controller *app, const char *token, yyjson_val *event) {
    if (!yyjson_is_obj(event)) return 0;
    pthread_mutex_lock(&app->state);
    session_entry *entry = controller_find(app, token);
    const char *type = yyjson_is_str(yyjson_obj_get(event, "type")) ? yyjson_get_str(yyjson_obj_get(event, "type")) : "";
    if (entry && strcmp(type, "agent-turn-complete") == 0) {
        const char *thread = yyjson_is_str(yyjson_obj_get(event, "thread-id")) ? yyjson_get_str(yyjson_obj_get(event, "thread-id")) : "";
        const char *turn = yyjson_is_str(yyjson_obj_get(event, "turn-id")) ? yyjson_get_str(yyjson_obj_get(event, "turn-id")) : "";
        int excluded = 0;
        for (size_t i = 0; i < entry->excluded_count; i++) if (strcmp(entry->excluded[i], thread) == 0) excluded = 1;
        if (excluded) {
            pthread_mutex_unlock(&app->state);
            return 0;
        }
        entry->has_candidate = thread[0] != 0;
        if (thread[0]) snprintf(entry->candidate, sizeof entry->candidate, "%s", thread);
        if (!app->has_token || strcmp(token, app->token) != 0) {
            if ((!entry->has_thread || strcmp(entry->thread, thread) == 0) && thread[0]) {
                if (!controller_has_turn(entry->turns, entry->turn_count, turn)) settle(entry, turn);
                entry->has_thread = 1;
                snprintf(entry->thread, sizeof entry->thread, "%s", thread);
                entry->draft = 0;
                char summary[2048];
                const char *raw = yyjson_is_str(yyjson_obj_get(event, "last-assistant-message")) ? yyjson_get_str(yyjson_obj_get(event, "last-assistant-message")) : "";
                if (spoken_text(raw, summary, sizeof summary) != 0) summary[0] = 0;
                free(entry->reply);
                entry->reply = summary[0] ? strdup(summary) : strdup("");
                controller_add_turn(&entry->turns, &entry->turn_count, &entry->turn_cap, turn);
                controller_save_selection(app);
            }
            pthread_mutex_unlock(&app->state);
            return 0;
        }
    }
    if (!app->target || !app->has_token || strcmp(token, app->token) != 0 || strcmp(type, "agent-turn-complete") != 0) {
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    const char *thread = yyjson_is_str(yyjson_obj_get(event, "thread-id")) ? yyjson_get_str(yyjson_obj_get(event, "thread-id")) : "";
    const char *turn = yyjson_is_str(yyjson_obj_get(event, "turn-id")) ? yyjson_get_str(yyjson_obj_get(event, "turn-id")) : "";
    if (!thread[0] || !turn[0] || (app->has_thread && strcmp(app->thread, thread) != 0)) {
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    if (controller_has_turn(app->turns, app->turn_count, turn)) {
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    if (entry) settle(entry, turn);
    app->has_thread = 1;
    snprintf(app->thread, sizeof app->thread, "%s", thread);
    controller_add_turn(&app->turns, &app->turn_count, &app->turn_cap, turn);
    controller_save_selection(app);
    app->draft = 0;
    if (strcmp(app->phase, "draft") == 0 && !app->pending) snprintf(app->phase, sizeof app->phase, "idle");
    char summary[2048];
    const char *raw = yyjson_is_str(yyjson_obj_get(event, "last-assistant-message")) ? yyjson_get_str(yyjson_obj_get(event, "last-assistant-message")) : "";
    if (spoken_text(raw, summary, sizeof summary) != 0) summary[0] = 0;
    free(app->reply);
    app->reply = summary[0] ? strdup(summary) : strdup("");
    int speak = app->auto_read && app->target && !app->target->data.team_child && app->reply[0]
        && !controller_recording_active(app) && !app->pending;
    pthread_mutex_unlock(&app->state);
    if (speak) {
        char err[CTRL_MSG];
        controller_read(app, 1, err, sizeof err);
    }
    return 1;
}

int controller_harness_event(voice_controller *app, const char *token, yyjson_val *event) {
    if (!yyjson_is_obj(event)) return 0;
    EventView ev;
    event_view_from(event, &ev);
    const char *kind = yyjson_is_str(yyjson_obj_get(event, "type")) ? yyjson_get_str(yyjson_obj_get(event, "type")) : "";
    pthread_mutex_lock(&app->state);
    session_entry *entry = controller_find(app, token);
    target_data copy;
    int managed = 0;
    if (entry && entry->target && entry->target->data.managed) {
        EventView tv = {0};
        tv.bridge_id = entry->target->data.bridge_id;
        tv.has_activation = entry->target->data.has_activation;
        tv.activation = entry->target->data.activation;
        tv.has_pid = 1;
        tv.pid = entry->target->data.pid;
        tv.session = entry->target->data.session;
        tv.harness = entry->target->data.harness;
        if (!event_matches(&tv, &ev, 1)) {
            pthread_mutex_unlock(&app->state);
            return 0;
        }
        copy = entry->target->data;
        managed = 1;
    }
    pthread_mutex_unlock(&app->state);
    if (managed && (strcmp(kind, "ready") == 0 || strcmp(kind, "heartbeat") == 0)) {
        target_data validated;
        char err[CTRL_MSG];
        if (controller_validate_attachment(app, &copy, &validated, err, sizeof err) != 0
            || validated.socket_device != copy.socket_device || validated.socket_inode != copy.socket_inode
            || validated.adapter_device != copy.adapter_device || validated.adapter_inode != copy.adapter_inode
            || strcmp(validated.start, copy.start) != 0) return 0;
        pthread_mutex_lock(&app->state);
        entry = controller_find(app, token);
        if (!entry) {
            pthread_mutex_unlock(&app->state);
            return 0;
        }
        if (strcmp(kind, "heartbeat") == 0 && (!entry->has_connection || strcmp(entry->connection_state, "ready") != 0)) {
            pthread_mutex_unlock(&app->state);
            return 0;
        }
        entry->heartbeat_at = controller_now(app);
        if (strcmp(kind, "ready") == 0) {
            snprintf(entry->connection_state, sizeof entry->connection_state, "ready");
            entry->has_connection = 1;
            int reconnect = app->has_reconnect && strcmp(app->reconnect_path, copy.socket) == 0
                && strcmp(app->reconnect_pane_id, copy.pane) == 0
                && app->reconnect_pane.socket_device == copy.socket_device
                && app->reconnect_pane.socket_inode == copy.socket_inode;
            if ((!app->has_token && reconnect) || can_auto_select(app->selection_initialized, app->has_token ? app->token : NULL, 1, copy.team_child)) {
                controller_load_selected(app, token, reconnect ? app->reconnect_explicit : 0);
                app->selection_initialized = 1;
            }
            if (app->has_token && strcmp(app->token, token) == 0) app->has_reconnect = 0;
            controller_save_selection(app);
        }
        pthread_mutex_unlock(&app->state);
        return 1;
    }
    pthread_mutex_lock(&app->state);
    entry = controller_find(app, token);
    if (managed && entry) {
        if (strcmp(kind, "metadata") == 0) {
            if (yyjson_is_str(yyjson_obj_get(event, "model"))) {
                copy_cap(entry->target->data.model, sizeof entry->target->data.model, yyjson_get_str(yyjson_obj_get(event, "model")));
                entry->target->data.has_model = 1;
            }
            if (yyjson_is_str(yyjson_obj_get(event, "thinking"))) {
                copy_cap(entry->target->data.thinking, sizeof entry->target->data.thinking, yyjson_get_str(yyjson_obj_get(event, "thinking")));
                entry->target->data.has_thinking = 1;
            }
            pthread_mutex_unlock(&app->state);
            return 1;
        }
        if (strcmp(kind, "shutdown") == 0) {
            controller_remove_session(app, token, 1);
            controller_save_selection(app);
            pthread_mutex_unlock(&app->state);
            return 1;
        }
        if (!entry->has_connection || strcmp(entry->connection_state, "ready") != 0) {
            pthread_mutex_unlock(&app->state);
            return 0;
        }
    }
    if (!entry || !yyjson_is_str(yyjson_obj_get(event, "harness")) || !yyjson_is_str(yyjson_obj_get(event, "session"))
        || strcmp(yyjson_get_str(yyjson_obj_get(event, "harness")), entry->target->data.harness) != 0) {
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    if (yyjson_is_int(yyjson_obj_get(event, "pid")) && yyjson_get_int(yyjson_obj_get(event, "pid")) != entry->target->data.pid) {
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    const char *session = yyjson_get_str(yyjson_obj_get(event, "session"));
    if (strcmp(kind, "session") == 0) {
        entry->has_candidate = 1;
        snprintf(entry->candidate, sizeof entry->candidate, "%s", session);
        if (!entry->has_thread) {
            entry->has_thread = 1;
            snprintf(entry->thread, sizeof entry->thread, "%s", session);
            entry->target->data.has_session = 1;
            entry->target->data.session_null = 0;
            copy_cap(entry->target->data.session, sizeof entry->target->data.session, session);
            if (app->has_token && strcmp(app->token, token) == 0) {
                app->has_thread = 1;
                snprintf(app->thread, sizeof app->thread, "%s", session);
            }
        }
        controller_save_selection(app);
        pthread_mutex_unlock(&app->state);
        return 1;
    }
    if (entry->has_thread && strcmp(entry->thread, session) != 0) {
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    if (strcmp(kind, "busy") == 0) {
        const char *state = yyjson_is_str(yyjson_obj_get(event, "state")) ? yyjson_get_str(yyjson_obj_get(event, "state")) : "";
        controller_set_activity(entry, strcmp(state, "blocked") == 0 ? "blocked" : "working");
        if (yyjson_is_str(yyjson_obj_get(event, "turn"))) {
            entry->has_active_turn = 1;
            snprintf(entry->active_turn, sizeof entry->active_turn, "%s", yyjson_get_str(yyjson_obj_get(event, "turn")));
        }
        pthread_mutex_unlock(&app->state);
        return 1;
    }
    if (strcmp(kind, "settled") == 0 || strcmp(kind, "shutdown") == 0) {
        const char *turn = yyjson_is_str(yyjson_obj_get(event, "turn")) ? yyjson_get_str(yyjson_obj_get(event, "turn")) : NULL;
        settle(entry, turn);
        int flush = strcmp(kind, "settled") == 0 && app->has_token && strcmp(app->token, token) == 0;
        pthread_mutex_unlock(&app->state);
        if (flush) {
            char err[CTRL_MSG];
            pthread_mutex_lock(&app->state);
            int queued = app->send_when_idle && (app->draft || app->pending) && !controller_recording_active(app);
            pthread_mutex_unlock(&app->state);
            if (queued) controller_send(app, NULL, 0, 1, err, sizeof err);
        }
        return 1;
    }
    if (strcmp(kind, "reply") != 0 || !yyjson_is_str(yyjson_obj_get(event, "turn"))) {
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    pthread_mutex_unlock(&app->state);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "type", "agent-turn-complete");
    yyjson_mut_obj_add_strcpy(doc, root, "thread-id", session);
    yyjson_mut_obj_add_strcpy(doc, root, "turn-id", yyjson_get_str(yyjson_obj_get(event, "turn")));
    yyjson_mut_obj_add_strcpy(doc, root, "last-assistant-message", yyjson_is_str(yyjson_obj_get(event, "text")) ? yyjson_get_str(yyjson_obj_get(event, "text")) : "");
    yyjson_doc *owned = yyjson_mut_doc_imut_copy(doc, NULL);
    yyjson_mut_doc_free(doc);
    int notified = controller_notify(app, token, yyjson_doc_get_root(owned));
    yyjson_doc_free(owned);
    return managed ? 1 : notified;
}

int controller_register(voice_controller *app, const char *token, yyjson_val *target_json, char *err, size_t cap) {
    if (!token || !token[0] || strlen(token) >= CTRL_TOKEN) {
        snprintf(err, cap, "Invalid voice token");
        return -1;
    }
    target_data parsed;
    if (controller_parse_target(target_json, &parsed, err, cap) != 0) return -1;
    pthread_mutex_lock(&app->state);
    int lost = app->target && app->target->data.managed;
    if (strcmp(parsed.harness, "pi") == 0 || strcmp(parsed.harness, "qwen-pi") == 0 || parsed.harness[0] == 0) {
        for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
            session_entry *existing = &app->sessions[i];
            if (!existing->used || !existing->target || !existing->target->data.managed) continue;
            if (strcmp(existing->target->data.pane, parsed.pane) == 0 && strcmp(existing->target->data.socket, parsed.socket) == 0)
                controller_remove_session(app, existing->token, 1);
        }
    }
    controller_remember(app);
    if (lost && app->target) {
        if (controller_retain(app, app->pending, app->token, &app->target->data, NULL)) {
            session_entry *cur = controller_find(app, app->token);
            if (cur) {
                free(cur->pending);
                cur->pending = NULL;
            }
        }
        if (app->record_op) app->record_op->voice_target_lost = 1;
    }
    pthread_mutex_unlock(&app->state);
    controller_stop(app, 1, lost);
    pthread_mutex_lock(&app->state);
    if (parsed.harness[0] == 0) snprintf(parsed.harness, sizeof parsed.harness, "pi");
    parsed.has_session = 1;
    parsed.session_null = 1;
    parsed.session[0] = 0;
    snprintf(parsed.token, sizeof parsed.token, "%s", token);
    session_entry *slot = session_slot_pub(app);
    if (!slot) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Too many voice sessions");
        return -1;
    }
    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    snprintf(slot->token, sizeof slot->token, "%s", token);
    slot->target = controller_snap(&parsed);
    snprintf(slot->agent_state, sizeof slot->agent_state, "unknown");
    controller_population(app);
    app->selection_initialized = 1;
    app->has_reconnect = 0;
    controller_load_selected(app, token, 1);
    controller_save_selection(app);
    pthread_mutex_unlock(&app->state);
    return 0;
}

int controller_select(voice_controller *app, const char *token, char *err, size_t cap) {
    char req[CTRL_TOKEN + 32];
    snprintf(req, sizeof req, "{\"action\":\"select:%s\"}", token ? token : "");
    char *response = controller_dispatch(app, req);
    yyjson_doc *doc = response ? yyjson_read(response, strlen(response), 0) : NULL;
    int ok = doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok"));
    if (!ok) {
        const char *message = doc && yyjson_is_str(yyjson_obj_get(yyjson_doc_get_root(doc), "error"))
            ? yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "error")) : "That voice session is no longer available";
        snprintf(err, cap, "%s", message);
    }
    yyjson_doc_free(doc);
    free(response);
    return ok ? 0 : -1;
}

void controller_refresh_reconnect(voice_controller *app) {
    pthread_mutex_lock(&app->state);
    int has = app->has_reconnect;
    char path[CTRL_PATH];
    uint64_t dev = app->reconnect_pane.socket_device, ino = app->reconnect_pane.socket_inode;
    snprintf(path, sizeof path, "%s", app->reconnect_path);
    pthread_mutex_unlock(&app->state);
    if (!has) return;
    char canonical[CTRL_PATH];
    uint64_t ndev = 0, nino = 0;
    if (socket_instance(path, canonical, sizeof canonical, &ndev, &nino) != 0) return;
    if (ndev != dev || nino != ino) return;
    if (app->deps.has_catalogue && app->deps.catalogue.read_panes) {
        int auth = 0;
        char **ids = NULL;
        size_t count = 0;
        if (app->deps.catalogue.read_panes(app->deps.catalogue.user, path, ndev, nino, &auth, &ids, &count) == 0 && auth) {
            pthread_mutex_lock(&app->state);
            int drop = app->has_reconnect && strcmp(app->reconnect_path, path) == 0
                && app->reconnect_pane.socket_device == dev && app->reconnect_pane.socket_inode == ino;
            if (drop) {
                int found = 0;
                for (size_t i = 0; i < count; i++) if (strcmp(ids[i], app->reconnect_pane_id) == 0) found = 1;
                if (!found) {
                    app->has_reconnect = 0;
                    app->reconnect_explicit = 0;
                    controller_save_selection(app);
                }
            }
            pthread_mutex_unlock(&app->state);
        }
        for (size_t i = 0; i < count; i++) free(ids[i]);
        free(ids);
        return;
    }
    target_data probe = {0};
    snprintf(probe.socket, sizeof probe.socket, "%s", path);
    probe.managed = 1;
    snprintf(probe.harness, sizeof probe.harness, "pi");
    const char *argv[] = {"api", "snapshot", NULL};
    yyjson_doc *doc = NULL;
    char err[CTRL_MSG];
    if (herdr_result(app, &probe, argv, &doc, err, sizeof err) != 0) return;
    yyjson_val *snapshot = yyjson_obj_get(yyjson_doc_get_root(doc), "snapshot");
    yyjson_val *panes = snapshot ? yyjson_obj_get(snapshot, "panes") : NULL;
    uint64_t sdev = 0, sino = 0;
    char again[CTRL_PATH];
    if (!yyjson_is_arr(panes) || socket_instance(path, again, sizeof again, &sdev, &sino) != 0 || sdev != ndev || sino != nino) {
        yyjson_doc_free(doc);
        return;
    }
    pthread_mutex_lock(&app->state);
    if (app->has_reconnect && strcmp(app->reconnect_path, path) == 0) {
        int found = 0;
        size_t idx, max;
        yyjson_val *row;
        yyjson_arr_foreach(panes, idx, max, row) {
            if (yyjson_is_obj(row) && yyjson_is_str(yyjson_obj_get(row, "pane_id"))
                && strcmp(yyjson_get_str(yyjson_obj_get(row, "pane_id")), app->reconnect_pane_id) == 0) found = 1;
        }
        if (!found) {
            app->has_reconnect = 0;
            app->reconnect_explicit = 0;
            controller_save_selection(app);
        }
    }
    pthread_mutex_unlock(&app->state);
    yyjson_doc_free(doc);
}

void controller_refresh_labels(voice_controller *app) {
    if (!app->deps.has_catalogue || !app->deps.catalogue.set_active) return;
    pthread_mutex_lock(&app->state);
    char paths[CTRL_MAX_SESSIONS][CTRL_PATH];
    uint64_t devices[CTRL_MAX_SESSIONS], inodes[CTRL_MAX_SESSIONS];
    const char *pp[CTRL_MAX_SESSIONS];
    size_t n = 0;
    if (app->has_reconnect && n < (size_t)app->deps.catalogue.max_keys) {
        snprintf(paths[n], sizeof paths[n], "%s", app->reconnect_path);
        devices[n] = app->reconnect_pane.socket_device;
        inodes[n] = app->reconnect_pane.socket_inode;
        pp[n] = paths[n];
        n++;
    }
    for (int i = 0; i < CTRL_MAX_SESSIONS && n < (size_t)app->deps.catalogue.max_keys; i++) {
        session_entry *entry = &app->sessions[i];
        if (!entry->used || !entry->target) continue;
        if (strcmp(entry->target->data.harness, "pi") != 0 && strcmp(entry->target->data.harness, "qwen-pi") != 0) continue;
        snprintf(paths[n], sizeof paths[n], "%s", entry->target->data.socket);
        devices[n] = entry->target->data.socket_device;
        inodes[n] = entry->target->data.socket_inode;
        pp[n] = paths[n];
        n++;
    }
    pthread_mutex_unlock(&app->state);
    app->deps.catalogue.set_active(app->deps.catalogue.user, pp, devices, inodes, n);
    for (size_t i = 0; i < n; i++) app->deps.catalogue.refresh(app->deps.catalogue.user, paths[i], devices[i], inodes[i]);
}

int controller_monitor(voice_controller *app) {
    double now = controller_now(app);
    pthread_mutex_lock(&app->state);
    Fences fences = {0};
    registry_fences(app->attachments, &fences);
    int pids[128];
    char starts[128][CTRL_START];
    size_t np = 0;
    for (size_t i = 0; i < fences.highwater_count && np < 128; i++) pids[np++] = fences.highwater[i].pid;
    for (size_t i = 0; i < fences.retired_count && np < 128; i++) pids[np++] = fences.retired[i].pid;
    fences_free(&fences);
    target_data copies[CTRL_MAX_SESSIONS];
    char tokens[CTRL_MAX_SESSIONS][CTRL_TOKEN];
    int managed[CTRL_MAX_SESSIONS];
    double hearts[CTRL_MAX_SESSIONS];
    int n = 0;
    for (int i = 0; i < CTRL_MAX_SESSIONS; i++) {
        if (!app->sessions[i].used || !app->sessions[i].target) continue;
        copies[n] = app->sessions[i].target->data;
        snprintf(tokens[n], sizeof tokens[n], "%s", app->sessions[i].token);
        managed[n] = app->sessions[i].target->data.managed;
        hearts[n] = app->sessions[i].heartbeat_at;
        n++;
    }
    pthread_mutex_unlock(&app->state);
    ObservedStart observed[128];
    size_t no = 0;
    for (size_t i = 0; i < np; i++) {
        int seen = 0;
        for (size_t j = 0; j < no; j++) if (observed[j].pid == pids[i]) seen = 1;
        if (seen) continue;
        observed[no].pid = pids[i];
        if (process_start_ticks(pids[i], starts[no], sizeof starts[no]) != 0) observed[no].start = NULL;
        else observed[no].start = starts[no];
        no++;
    }
    int dead[CTRL_MAX_SESSIONS] = {0};
    int expired[CTRL_MAX_SESSIONS] = {0};
    for (int i = 0; i < n; i++) {
        char start[CTRL_START];
        int gone = process_start_ticks(copies[i].pid, start, sizeof start) != 0 || !copies[i].has_start || strcmp(start, copies[i].start) != 0;
        if (managed[i]) {
            int lease = now - hearts[i] >= LEASE_SECONDS;
            if (!gone && !lease) {
                target_data validated;
                char err[CTRL_MSG];
                if (controller_validate_attachment(app, &copies[i], &validated, err, sizeof err) != 0
                    || strcmp(validated.start, copies[i].start) != 0
                    || validated.socket_device != copies[i].socket_device
                    || validated.socket_inode != copies[i].socket_inode
                    || validated.adapter_device != copies[i].adapter_device
                    || validated.adapter_inode != copies[i].adapter_inode) gone = 1;
            }
            if (lease || gone) {
                expired[i] = lease;
                dead[i] = gone;
            }
        } else if (gone) dead[i] = 1;
    }
    int removed = 0;
    pthread_mutex_lock(&app->state);
    for (int i = 0; i < n; i++) {
        if (!dead[i] && !expired[i]) continue;
        session_entry *entry = controller_find(app, tokens[i]);
        if (!entry) continue;
        if (!dead[i] && now - entry->heartbeat_at < LEASE_SECONDS) continue;
        controller_remove_session(app, tokens[i], 0);
        removed++;
    }
    CapturedIncarnation captured[128];
    size_t nc = 0;
    for (size_t i = 0; i < no && nc < 128; i++) {
        captured[nc].pid = observed[i].pid;
        captured[nc].start = observed[i].start;
        nc++;
    }
    registry_prune(app->attachments, observed, no, captured, nc, 1);
    controller_expire_retry(app, 0);
    if (removed) controller_save_selection(app);
    pthread_mutex_unlock(&app->state);
    controller_refresh_reconnect(app);
    controller_refresh_labels(app);
    if (app->deps.has_engines && app->deps.engines.sweep) app->deps.engines.sweep(app->deps.engines.user);
    return removed;
}
