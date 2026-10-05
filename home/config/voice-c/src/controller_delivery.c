#define _POSIX_C_SOURCE 200809L
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "runtime_adapters.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void controller_delivery_outcome(voice_controller *app, const char *token, const char *text, op_state *operation, op_state *cancelled, const char *outcome) {
    pthread_mutex_lock(&app->state);
    if (operation) snprintf(operation->delivery_outcome, sizeof operation->delivery_outcome, "%s", outcome);
    if (cancelled) snprintf(cancelled->delivery_outcome, sizeof cancelled->delivery_outcome, "%s", outcome);
    if (app->retained && token && strcmp(app->retained->source_token, token) == 0 && text && strcmp(app->retained->text, text) == 0) {
        if (strcmp(outcome, "accepted") == 0) {
            free(app->retained->text);
            free(app->retained);
            app->retained = NULL;
        } else app->recovery_uncertain = 1;
    }
    pthread_mutex_unlock(&app->state);
}

static int map_terminal(int rc) {
    if (rc == VOICE_OK || rc == CTRL_OK) return CTRL_OK;
    if (rc == VOICE_UNCERTAIN || rc == CTRL_UNCERTAIN) return CTRL_UNCERTAIN;
    if (rc == VOICE_CANCELLED || rc == CTRL_CANCELLED) return CTRL_CANCELLED;
    return CTRL_ERR;
}

int controller_stage(voice_controller *app, const char *text, const char *token, op_state *cancelled, int finish, char *err, size_t cap) {
    char cleaned[256 * 1024];
    if (dictation_text(text, cleaned, sizeof cleaned) != 0) {
        snprintf(err, cap, "Whisper returned an invalid transcript");
        return CTRL_ERR;
    }
    pthread_mutex_lock(&app->state);
    controller_require_ready(app, token, err, cap);
    if (err[0]) {
        pthread_mutex_unlock(&app->state);
        return CTRL_ERR;
    }
    if (!app->target || !app->has_token || strcmp(token, app->token) != 0) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Voice session changed; discarded the old transcription");
        return CTRL_ERR;
    }
    if (!cleaned[0]) {
        app->draft = 0;
        free(app->pending);
        app->pending = NULL;
        if (finish) snprintf(app->phase, sizeof app->phase, "idle");
        pthread_mutex_unlock(&app->state);
        return CTRL_OK;
    }
    if (has_control_characters(cleaned)) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Discarded dictation containing terminal control characters");
        return CTRL_ERR;
    }
    target_snap *snap = controller_snap_retain(app->target);
    op_state *operation = controller_op_retain(app->input_op);
    session_entry *entry = controller_find(app, token);
    if (entry) entry->draft_revision++;
    pthread_mutex_unlock(&app->state);
    if (pthread_mutex_trylock(&app->delivery) != 0) {
        controller_snap_release(snap);
        controller_op_release(operation);
        snprintf(err, cap, "Input delivery is already in progress");
        return CTRL_ERR;
    }
    int rc = CTRL_OK;
    if (atomic_load(&operation->cancelled) || (cancelled && atomic_load(&cancelled->cancelled))) {
        rc = CTRL_OK;
        goto unlock;
    }
    controller_target_view view;
    controller_view(&snap->data, &view);
    if (app->deps.terminal.validate_target) {
        char probe[CTRL_MSG] = {0};
        if (map_terminal(app->deps.terminal.validate_target(app->deps.terminal.user, &view, probe, sizeof probe)) != CTRL_OK) {
            pthread_mutex_lock(&app->state);
            session_entry *current = controller_find(app, snap->data.token);
            if (current && controller_same_target(&current->target->data, &snap->data)) {
                controller_remove_session(app, snap->data.token, 0);
                controller_save_selection(app);
            }
            pthread_mutex_unlock(&app->state);
            snprintf(err, cap, "Pi voice disconnected; reconnect before delivery");
            rc = CTRL_ERR;
            goto unlock;
        }
    }
    if (atomic_load(&operation->cancelled) || (cancelled && atomic_load(&cancelled->cancelled))) goto unlock;
    if (app->deps.terminal.insert_guarded) {
        char insert_err[CTRL_MSG] = {0};
        int inserted = map_terminal(app->deps.terminal.insert_guarded(
            app->deps.terminal.user, &view, cleaned, &operation->cancelled, insert_err, sizeof insert_err));
        if (inserted == CTRL_UNCERTAIN) {
            controller_delivery_outcome(app, token, cleaned, operation, cancelled, "unknown");
            pthread_mutex_lock(&app->state);
            if (app->has_token && strcmp(token, app->token) == 0 && operation == app->input_op) {
                free(app->pending);
                app->pending = NULL;
                app->draft = 0;
            }
            pthread_mutex_unlock(&app->state);
            snprintf(err, cap, "%s", insert_err[0] ? insert_err : "Could not confirm delivery; check the prompt");
            rc = CTRL_UNCERTAIN;
            goto unlock;
        }
        if (inserted != CTRL_OK) {
            snprintf(err, cap, "%s", insert_err[0] ? insert_err : "Could not paste into the selected Pi pane");
            rc = inserted;
            goto unlock;
        }
    }
    controller_delivery_outcome(app, token, cleaned, operation, cancelled, "accepted");
    pthread_mutex_lock(&app->state);
    if (!app->has_token || strcmp(token, app->token) != 0 || atomic_load(&operation->cancelled)
        || (cancelled && atomic_load(&cancelled->cancelled))) {
        pthread_mutex_unlock(&app->state);
        goto unlock;
    }
    app->draft = 1;
    free(app->pending);
    app->pending = NULL;
    entry = controller_find(app, token);
    if (entry) {
        entry->draft_revision++;
        snprintf(entry->draft_state, sizeof entry->draft_state, "staged");
    }
    if (finish) snprintf(app->phase, sizeof app->phase, "draft");
    free(app->error);
    app->error = NULL;
    pthread_mutex_unlock(&app->state);
    rc = 1;
unlock:
    pthread_mutex_unlock(&app->delivery);
    controller_snap_release(snap);
    controller_op_release(operation);
    return rc;
}

static int queueable(const char *message) {
    if (!message) return 0;
    return strcasestr(message, "busy") || strcasestr(message, "working") || strcasestr(message, "waiting for an interaction");
}

int controller_send(voice_controller *app, const char *expected_token, uint64_t expected_gen, int allow_edited, char *err, size_t cap) {
    err[0] = 0;
    pthread_mutex_lock(&app->state);
    controller_require_ready(app, app->token, err, cap);
    if (err[0]) {
        pthread_mutex_unlock(&app->state);
        return CTRL_ERR;
    }
    if (expected_token && (!app->has_token || strcmp(expected_token, app->token) != 0 || !app->input_op || app->input_op->generation != expected_gen)) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Voice session changed; press Send again");
        return CTRL_ERR;
    }
    if (expected_token && app->input_op && atomic_load(&app->input_op->cancelled)) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Voice delivery was cancelled");
        return CTRL_ERR;
    }
    if (controller_recording_active(app)) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Finish recording and transcription before sending");
        return CTRL_ERR;
    }
    char *pending = app->pending ? strdup(app->pending) : NULL;
    char token[CTRL_TOKEN];
    snprintf(token, sizeof token, "%s", app->has_token ? app->token : "");
    pthread_mutex_unlock(&app->state);
    if (pending) {
        int staged = controller_stage(app, pending, token, NULL, 1, err, cap);
        free(pending);
        if (staged < 0) return staged;
    }
    pthread_mutex_lock(&app->state);
    if (expected_token && (!app->has_token || strcmp(expected_token, app->token) != 0 || !app->input_op || app->input_op->generation != expected_gen || atomic_load(&app->input_op->cancelled))) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Voice delivery was cancelled");
        return CTRL_ERR;
    }
    if (!app->target || !app->draft) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "No new dictation to send");
        return CTRL_ERR;
    }
    if (app->input_op && atomic_load(&app->input_op->cancelled)) {
        controller_op_release(app->input_op);
        app->input_op = controller_op_new(app);
    }
    target_snap *snap = controller_snap_retain(app->target);
    op_state *operation = controller_op_retain(app->input_op);
    pthread_mutex_unlock(&app->state);
    if (pthread_mutex_trylock(&app->delivery) != 0) {
        controller_snap_release(snap);
        controller_op_release(operation);
        snprintf(err, cap, "Input delivery is already in progress");
        return CTRL_ERR;
    }
    controller_target_view view;
    controller_view(&snap->data, &view);
    char call_err[CTRL_MSG] = {0};
    int rc = CTRL_OK;
    if (app->deps.terminal.validate) rc = map_terminal(app->deps.terminal.validate(app->deps.terminal.user, &view, call_err, sizeof call_err));
    if (rc == CTRL_OK) {
        pthread_mutex_lock(&app->state);
        if (!app->has_token || strcmp(token, app->token) != 0 || atomic_load(&operation->cancelled) || !app->draft) {
            pthread_mutex_unlock(&app->state);
            snprintf(call_err, sizeof call_err, "Voice delivery was cancelled");
            rc = CTRL_ERR;
        } else {
            app->draft = 0;
            snprintf(app->phase, sizeof app->phase, "idle");
            app->send_when_idle = 0;
            pthread_mutex_unlock(&app->state);
            if (app->deps.terminal.submit_guarded)
                rc = map_terminal(app->deps.terminal.submit_guarded(app->deps.terminal.user, &view, &operation->cancelled, allow_edited, call_err, sizeof call_err));
        }
    }
    pthread_mutex_unlock(&app->delivery);
    controller_snap_release(snap);
    controller_op_release(operation);
    if (rc != CTRL_OK) {
        pthread_mutex_lock(&app->state);
        int keep = (app->draft || app->pending) && queueable(call_err);
        if (keep) {
            app->send_when_idle = 1;
            snprintf(app->phase, sizeof app->phase, "draft");
        }
        pthread_mutex_unlock(&app->state);
        if (keep) return CTRL_OK;
        snprintf(err, cap, "%s", call_err[0] ? call_err : "Could not send dictation");
        return rc;
    }
    return CTRL_OK;
}

typedef struct speak_job {
    voice_controller *app;
    char *text;
    op_state *op;
} speak_job;

static void speak_run(void *arg) {
    speak_job *job = arg;
    void *lease = NULL;
    const char *backend = job->app->deps.audio.speech_backend ? job->app->deps.audio.speech_backend(job->app->deps.audio.user) : "local";
    if (job->app->deps.has_engines && job->app->deps.engines.acquire && strcmp(backend, "local") == 0)
        job->app->deps.engines.acquire(job->app->deps.engines.user, "tts", &lease, (char[CTRL_MSG]){0}, CTRL_MSG);
    int ready = 1;
    while (lease && !atomic_load(&job->op->cancelled) && job->app->deps.engines.wait) {
        char err[CTRL_MSG] = {0};
        int wr = job->app->deps.engines.wait(lease, 100, err, sizeof err);
        if (wr == 0) break;
        if (wr == 1 && job->app->deps.engines.failed && job->app->deps.engines.failed(lease)) break;
        if (wr < 0) break;
    }
    if (atomic_load(&job->op->cancelled)) ready = 0;
    if (ready && job->app->deps.audio.speak) {
        char err[CTRL_MSG] = {0};
        if (job->app->deps.audio.speak(job->app->deps.audio.user, job->text, &job->op->cancelled, err, sizeof err) != 0
            && !atomic_load(&job->op->cancelled))
            controller_notice(job->app, "Could not read the reply", err, "red");
    }
    pthread_mutex_lock(&job->app->state);
    job->app->audible = 0;
    job->app->speaking = 0;
    pthread_mutex_unlock(&job->app->state);
    if (lease && job->app->deps.engines.release) job->app->deps.engines.release(lease);
    controller_op_release(job->op);
    free(job->text);
    free(job);
}

int controller_read(voice_controller *app, int replace, char *err, size_t cap) {
    pthread_mutex_lock(&app->state);
    if (app->target && app->target->data.team_child) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Team members are silent");
        return CTRL_ERR;
    }
    if (app->pending) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Send or cancel retained dictation first");
        return CTRL_ERR;
    }
    if (controller_recording_active(app)) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Finish recording and transcription before reading a reply");
        return CTRL_ERR;
    }
    int playing = app->speaking;
    char *text = app->reply ? strdup(app->reply) : NULL;
    pthread_mutex_unlock(&app->state);
    controller_stop(app, 0, 0);
    if (playing && !replace) {
        free(text);
        return CTRL_OK;
    }
    if (!text || !text[0]) {
        free(text);
        snprintf(err, cap, "No summary in the latest completed reply");
        return CTRL_ERR;
    }
    pthread_mutex_lock(&app->state);
    controller_op_release(app->speak_op);
    app->speak_op = controller_op_new(app);
    app->audible = 0;
    app->speaking = 1;
    speak_job *job = calloc(1, sizeof *job);
    job->app = app;
    job->text = text;
    job->op = controller_op_retain(app->speak_op);
    pthread_mutex_unlock(&app->state);
    if (controller_submit(app, speak_run, job) != 0) {
        pthread_mutex_lock(&app->state);
        app->speaking = 0;
        pthread_mutex_unlock(&app->state);
        controller_op_release(job->op);
        free(job->text);
        free(job);
        snprintf(err, cap, "Voice is busy");
        return CTRL_ERR;
    }
    return CTRL_OK;
}

int controller_recover_copy(voice_controller *app, char *err, size_t cap) {
    pthread_mutex_lock(&app->state);
    char *text = app->retained && app->retained->text ? strdup(app->retained->text) : NULL;
    pthread_mutex_unlock(&app->state);
    if (!text) {
        snprintf(err, cap, "No retained dictation");
        return CTRL_ERR;
    }
    int rc = app->deps.copy_text ? app->deps.copy_text(app->deps.copy_user, text, err, cap) : CTRL_ERR;
    free(text);
    if (!app->deps.copy_text) snprintf(err, cap, "Could not copy retained dictation; try again");
    return rc;
}

int controller_recover_discard(voice_controller *app, char *err, size_t cap) {
    pthread_mutex_lock(&app->state);
    if (app->recovery_inflight) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Wait for retained dictation staging to finish");
        return CTRL_ERR;
    }
    if (app->retained) {
        free(app->retained->text);
        free(app->retained);
        app->retained = NULL;
    }
    app->recovery_uncertain = 0;
    app->recovery_revision++;
    if (app->record_op) app->record_op->voice_target_lost = 0;
    if (!controller_recording_active(app)) {
        free(app->error);
        app->error = NULL;
        snprintf(app->phase, sizeof app->phase, "%s", (app->draft || app->pending) ? "draft" : "idle");
    }
    pthread_mutex_unlock(&app->state);
    return CTRL_OK;
}

int controller_recover_stage(voice_controller *app, char *err, size_t cap) {
    pthread_mutex_lock(&app->state);
    if (!app->retained || app->recovery_inflight) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "No retained dictation available to stage");
        return CTRL_ERR;
    }
    if (app->recovery_uncertain) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Check the Pi prompt; copy or discard this uncertain dictation");
        return CTRL_ERR;
    }
    if (!app->target || (strcmp(app->target->data.harness, "pi") != 0 && strcmp(app->target->data.harness, "qwen-pi") != 0) || !app->selection_explicit) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Explicitly select a ready Pi session for recovery");
        return CTRL_ERR;
    }
    if (controller_recording_active(app) || app->pending || app->draft) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Finish or discard the selected draft first");
        return CTRL_ERR;
    }
    controller_require_ready(app, app->token, err, cap);
    if (err[0]) {
        pthread_mutex_unlock(&app->state);
        return CTRL_ERR;
    }
    char *text = strdup(app->retained->text);
    char token[CTRL_TOKEN];
    snprintf(token, sizeof token, "%s", app->token);
    controller_op_release(app->input_op);
    app->input_op = controller_op_new(app);
    op_state *cancelled = controller_op_retain(app->input_op);
    app->recovery_inflight = 1;
    pthread_mutex_unlock(&app->state);
    int staged = controller_stage(app, text, token, cancelled, 1, err, cap);
    pthread_mutex_lock(&app->state);
    int accepted = strcmp(cancelled->delivery_outcome, "accepted") == 0;
    if ((staged > 0 || accepted) && app->retained && strcmp(app->retained->text, text) == 0) {
        free(app->retained->text);
        free(app->retained);
        app->retained = NULL;
    }
    if (staged == CTRL_UNCERTAIN) app->recovery_uncertain = 1;
    app->recovery_inflight = 0;
    pthread_mutex_unlock(&app->state);
    controller_op_release(cancelled);
    free(text);
    return staged < 0 ? staged : CTRL_OK;
}

int controller_interact(voice_controller *app, char *err, size_t cap) {
    controller_request_osd(app, NULL, 6, NULL);
    pthread_mutex_lock(&app->state);
    int recording = controller_recording_active(app) || !(app->draft || app->pending);
    if (app->input_op && atomic_load(&app->input_op->cancelled)) {
        controller_op_release(app->input_op);
        app->input_op = controller_op_new(app);
    }
    char token[CTRL_TOKEN];
    uint64_t gen = app->input_op ? app->input_op->generation : 0;
    snprintf(token, sizeof token, "%s", app->has_token ? app->token : "");
    pthread_mutex_unlock(&app->state);
    int rc = recording ? controller_record(app, "append", err, cap) : controller_send(app, token, gen, 1, err, cap);
    if (rc != 0) controller_request_osd(app, err, 6, NULL);
    return rc;
}

int controller_rebind(voice_controller *app, char *err, size_t cap) {
    pthread_mutex_lock(&app->state);
    if (!app->target || !app->has_token) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Select a voice session first");
        return CTRL_ERR;
    }
    char origin[CTRL_TOKEN];
    snprintf(origin, sizeof origin, "%s", app->token);
    target_data origin_identity = app->target->data;
    int origin_revision = app->recovery_revision;
    pthread_mutex_unlock(&app->state);
    controller_stop(app, 1, 0);
    pthread_mutex_lock(&app->state);
    session_entry *entry = controller_find(app, origin);
    if (!entry || !app->has_token || strcmp(app->token, origin) != 0 || !app->target
        || app->recovery_revision != origin_revision + 1
        || !controller_same_attachment(&app->target->data, &origin_identity)) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Voice session changed; press Send again");
        return CTRL_ERR;
    }
    if (entry->has_candidate) {
        app->has_thread = 1;
        snprintf(app->thread, sizeof app->thread, "%s", entry->candidate);
        target_data updated = entry->target->data;
        updated.has_session = 1;
        updated.session_null = 0;
        snprintf(updated.session, sizeof updated.session, "%s", entry->candidate);
        controller_snap_release(entry->target);
        entry->target = controller_snap(&updated);
        controller_snap_release(app->target);
        app->target = controller_snap_retain(entry->target);
    }
    controller_set_activity(entry, "unknown");
    controller_clear_turns(&app->turns, &app->turn_count, &app->turn_cap);
    app->draft = 0;
    free(app->reply);
    app->reply = NULL;
    snprintf(app->phase, sizeof app->phase, "idle");
    controller_op_release(app->input_op);
    app->input_op = controller_op_new(app);
    controller_save_selection(app);
    pthread_mutex_unlock(&app->state);
    controller_notice(app, "Voice rebound", "Using the selected session's conversation", "orange");
    return CTRL_OK;
}
