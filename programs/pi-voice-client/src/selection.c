#define _POSIX_C_SOURCE 200809L
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void controller_save_selection(voice_controller *app) {
    controller_remember(app);
    char *text = controller_selection_json(app);
    if (!text) return;
    char tmp[CTRL_PATH], path[CTRL_PATH];
    snprintf(tmp, sizeof tmp, "%s/selection.tmp", app->runtime);
    snprintf(path, sizeof path, "%s/%s", app->runtime, VOICE_SELECTION_NAME);
    FILE *file = fopen(tmp, "w");
    if (!file) {
        free(text);
        return;
    }
    fputs(text, file);
    fclose(file);
    free(text);
    chmod(tmp, 0600);
    rename(tmp, path);
}

static int load_turns(yyjson_val *arr, char ***turns, size_t *count, size_t *cap) {
    controller_clear_turns(turns, count, cap);
    if (!yyjson_is_arr(arr)) return 0;
    size_t idx, max;
    yyjson_val *item;
    yyjson_arr_foreach(arr, idx, max, item) {
        if (!yyjson_is_str(item)) continue;
        if (controller_add_turn(turns, count, cap, yyjson_get_str(item)) < 0) return -1;
    }
    return 0;
}

int controller_restore(voice_controller *app) {
    pthread_mutex_lock(&app->state);
    controller_snap_release(app->target);
    app->target = NULL;
    app->has_token = 0;
    app->token[0] = 0;
    char path[CTRL_PATH];
    snprintf(path, sizeof path, "%s/%s", app->runtime, VOICE_SELECTION_NAME);
    yyjson_doc *doc = yyjson_read_file(path, 0, NULL, NULL);
    if (!doc) {
        controller_population(app);
        pthread_mutex_unlock(&app->state);
        return 0;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    int ok = 0;
    if (!yyjson_is_obj(root) || !yyjson_obj_get(root, "target")) goto finish;
    app->show_team = yyjson_is_true(yyjson_obj_get(root, "show_team"));
    yyjson_val *token = yyjson_obj_get(root, "token");
    int has_saved_token = yyjson_is_str(token) && yyjson_get_str(token)[0];
    app->selection_initialized = yyjson_obj_get(root, "selection_initialized")
        ? yyjson_is_true(yyjson_obj_get(root, "selection_initialized"))
        : has_saved_token || !yyjson_is_null(yyjson_obj_get(root, "target"));
    app->selection_explicit = yyjson_obj_get(root, "selection_explicit")
        ? yyjson_is_true(yyjson_obj_get(root, "selection_explicit")) : has_saved_token;
    yyjson_val *reconnect = yyjson_obj_get(root, "reconnect_pane");
    app->has_reconnect = 0;
    if (yyjson_is_obj(reconnect) && yyjson_is_str(yyjson_obj_get(reconnect, "socket_path")) && yyjson_is_str(yyjson_obj_get(reconnect, "pane_id"))) {
        app->has_reconnect = 1;
        snprintf(app->reconnect_path, sizeof app->reconnect_path, "%s", yyjson_get_str(yyjson_obj_get(reconnect, "socket_path")));
        snprintf(app->reconnect_pane_id, sizeof app->reconnect_pane_id, "%s", yyjson_get_str(yyjson_obj_get(reconnect, "pane_id")));
        app->reconnect_pane.socket_path = app->reconnect_path;
        app->reconnect_pane.pane_id = app->reconnect_pane_id;
        app->reconnect_pane.socket_device = yyjson_is_uint(yyjson_obj_get(reconnect, "socket_device")) ? yyjson_get_uint(yyjson_obj_get(reconnect, "socket_device")) : 0;
        app->reconnect_pane.socket_inode = yyjson_is_uint(yyjson_obj_get(reconnect, "socket_inode")) ? yyjson_get_uint(yyjson_obj_get(reconnect, "socket_inode")) : 0;
    }
    app->reconnect_explicit = yyjson_is_true(yyjson_obj_get(root, "reconnect_explicit"));
    registry_free(app->attachments);
    app->attachments = registry_new();
    yyjson_val *fences = yyjson_obj_get(root, "attachment_fences");
    if (yyjson_is_obj(fences) && app->attachments) {
        Fences saved = {0};
        yyjson_val *high = yyjson_obj_get(fences, "highwater");
        yyjson_val *retired = yyjson_obj_get(fences, "retired");
        size_t nh = yyjson_is_arr(high) ? yyjson_arr_size(high) : 0;
        size_t nr = yyjson_is_arr(retired) ? yyjson_arr_size(retired) : 0;
        saved.highwater = calloc(nh ? nh : 1, sizeof *saved.highwater);
        saved.retired = calloc(nr ? nr : 1, sizeof *saved.retired);
        if (saved.highwater) {
            size_t idx, max;
            yyjson_val *row;
            yyjson_arr_foreach(high, idx, max, row) {
                if (!yyjson_is_arr(row) || yyjson_arr_size(row) < 4) continue;
                saved.highwater[saved.highwater_count].pid = (int)yyjson_get_int(yyjson_arr_get(row, 0));
                saved.highwater[saved.highwater_count].process_start = yyjson_get_str(yyjson_arr_get(row, 1));
                saved.highwater[saved.highwater_count].ordinal = yyjson_get_sint(yyjson_arr_get(row, 2));
                saved.highwater[saved.highwater_count].bridge_id = yyjson_get_str(yyjson_arr_get(row, 3));
                saved.highwater_count++;
            }
        }
        if (saved.retired) {
            size_t idx, max;
            yyjson_val *row;
            yyjson_arr_foreach(retired, idx, max, row) {
                if (!yyjson_is_arr(row) || yyjson_arr_size(row) < 3) continue;
                saved.retired[saved.retired_count].pid = (int)yyjson_get_int(yyjson_arr_get(row, 0));
                saved.retired[saved.retired_count].process_start = yyjson_get_str(yyjson_arr_get(row, 1));
                saved.retired[saved.retired_count].bridge_id = yyjson_get_str(yyjson_arr_get(row, 2));
                saved.retired_count++;
            }
        }
        registry_restore_fences(app->attachments, &saved);
        free(saved.highwater);
        free(saved.retired);
    }
    target_data saved_target;
    char parse_err[CTRL_MSG];
    int have_target = yyjson_is_obj(yyjson_obj_get(root, "target"))
        && controller_parse_target(yyjson_obj_get(root, "target"), &saved_target, parse_err, sizeof parse_err) == 0;
    if (have_target && saved_target.managed) {
        app->has_reconnect = 1;
        snprintf(app->reconnect_path, sizeof app->reconnect_path, "%s", saved_target.socket);
        snprintf(app->reconnect_pane_id, sizeof app->reconnect_pane_id, "%s", saved_target.pane);
        app->reconnect_pane.socket_path = app->reconnect_path;
        app->reconnect_pane.pane_id = app->reconnect_pane_id;
        app->reconnect_pane.socket_device = saved_target.socket_device;
        app->reconnect_pane.socket_inode = saved_target.socket_inode;
        app->reconnect_explicit = app->selection_explicit;
    }
    yyjson_val *sessions = yyjson_obj_get(root, "sessions");
    if (yyjson_is_obj(sessions)) {
        yyjson_val *key, *entry;
        yyjson_obj_iter iter = yyjson_obj_iter_with(sessions);
        while ((key = yyjson_obj_iter_next(&iter))) {
            entry = yyjson_obj_iter_get_val(key);
            const char *token_key = yyjson_get_str(key);
            target_data existing;
            if (!yyjson_is_obj(entry) || controller_parse_target(yyjson_obj_get(entry, "target"), &existing, parse_err, sizeof parse_err) != 0)
                continue;
            char start[CTRL_START];
            if (!existing.has_start || process_start_ticks(existing.pid, start, sizeof start) != 0 || strcmp(start, existing.start) != 0)
                continue;
            if (existing.managed) {
                AttachmentTarget at = {0};
                at.socket = existing.socket;
                at.socket_device = existing.socket_device;
                at.socket_inode = existing.socket_inode;
                at.pane = existing.pane;
                at.pid = existing.pid;
                at.start = existing.start;
                at.bridge_id = existing.bridge_id;
                BridgeIdentity identity = bridge_identity(&at);
                PaneKey pane = pane_key(&at);
                Admission admission;
                if (registry_admit(app->attachments, &identity, existing.activation, registry_revision(app->attachments, &pane), token_key, &admission) != 0
                    || !admission.state || strcmp(admission.state, "admitted") != 0) continue;
                session_entry *slot = session_slot_pub(app);
                if (!slot) continue;
                memset(slot, 0, sizeof *slot);
                slot->used = 1;
                snprintf(slot->token, sizeof slot->token, "%s", admission.token);
                snprintf(existing.token, sizeof existing.token, "%s", admission.token);
                slot->target = controller_snap(&existing);
                snprintf(slot->connection_state, sizeof slot->connection_state, "connecting");
                slot->has_connection = 1;
                slot->heartbeat_at = controller_now(app);
                snprintf(slot->agent_state, sizeof slot->agent_state, "unknown");
                yyjson_val *thread = yyjson_obj_get(entry, "thread");
                if (yyjson_is_str(thread)) {
                    slot->has_thread = 1;
                    snprintf(slot->thread, sizeof slot->thread, "%s", yyjson_get_str(thread));
                }
                yyjson_val *candidate = yyjson_obj_get(entry, "candidate");
                if (yyjson_is_str(candidate)) {
                    slot->has_candidate = 1;
                    snprintf(slot->candidate, sizeof slot->candidate, "%s", yyjson_get_str(candidate));
                }
                load_turns(yyjson_obj_get(entry, "turns"), &slot->turns, &slot->turn_count, &slot->turn_cap);
            } else {
                session_entry *slot = session_slot_pub(app);
                if (!slot) continue;
                memset(slot, 0, sizeof *slot);
                slot->used = 1;
                snprintf(slot->token, sizeof slot->token, "%s", token_key);
                snprintf(existing.token, sizeof existing.token, "%s", token_key);
                slot->target = controller_snap(&existing);
                snprintf(slot->agent_state, sizeof slot->agent_state, "unknown");
                load_turns(yyjson_obj_get(entry, "turns"), &slot->turns, &slot->turn_count, &slot->turn_cap);
                yyjson_val *thread = yyjson_obj_get(entry, "thread");
                if (yyjson_is_str(thread)) {
                    slot->has_thread = 1;
                    snprintf(slot->thread, sizeof slot->thread, "%s", yyjson_get_str(thread));
                }
            }
            controller_population(app);
        }
    }
    char live[CTRL_START];
    if (!have_target || !saved_target.has_start || process_start_ticks(saved_target.pid, live, sizeof live) != 0
        || strcmp(live, saved_target.start) != 0) goto finish;
    if (has_saved_token && controller_find(app, yyjson_get_str(token))) {
        controller_load_selected(app, yyjson_get_str(token), app->selection_explicit);
        ok = 1;
    } else if (!saved_target.managed && has_saved_token) {
        yyjson_val *target_json = yyjson_obj_get(root, "target");
        char saved_token[CTRL_TOKEN];
        snprintf(saved_token, sizeof saved_token, "%s", yyjson_get_str(token));
        pthread_mutex_unlock(&app->state);
        int registered = controller_register(app, saved_token, target_json, (char[CTRL_MSG]){0}, CTRL_MSG);
        yyjson_doc_free(doc);
        return registered == 0;
    }
finish:
    controller_population(app);
    pthread_mutex_unlock(&app->state);
    yyjson_doc_free(doc);
    return ok;
}
