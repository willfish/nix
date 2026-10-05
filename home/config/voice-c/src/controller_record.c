#define _POSIX_C_SOURCE 200809L
#define VOICE_CONTROLLER_INTERNAL
#include "controller.h"
#include "protocol.h"
#include "text.h"

#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int has_audio_file(const char *path, char *err, size_t cap) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(err, cap, "Expected 16 kHz mono PCM recording");
        return -1;
    }
    unsigned char header[44];
    if (fread(header, 1, 44, file) != 44 || memcmp(header, "RIFF", 4) != 0) {
        fclose(file);
        snprintf(err, cap, "Expected 16 kHz mono PCM recording");
        return -1;
    }
    uint16_t channels = header[22] | (header[23] << 8);
    uint32_t rate = header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24);
    uint16_t width = header[34] | (header[35] << 8);
    if (channels != 1 || width != 16 || rate != 16000) {
        fclose(file);
        snprintf(err, cap, "Expected 16 kHz mono PCM recording");
        return -1;
    }
    int16_t *samples = NULL;
    size_t count = 0, room = 0;
    int16_t buf[1024];
    size_t got;
    while ((got = fread(buf, sizeof(int16_t), 1024, file)) > 0) {
        if (count + got > room) {
            room = room ? room * 2 : 4096;
            int16_t *grown = realloc(samples, room * sizeof *samples);
            if (!grown) {
                free(samples);
                fclose(file);
                return -1;
            }
            samples = grown;
        }
        memcpy(samples + count, buf, got * sizeof(int16_t));
        count += got;
    }
    fclose(file);
    if (count < 3200) {
        free(samples);
        return 0;
    }
    int voiced = 0;
    for (size_t i = 0; i < count; i += 320) {
        size_t n = count - i < 320 ? count - i : 320;
        double sum = 0;
        for (size_t j = 0; j < n; j++) sum += (double)samples[i + j] * samples[i + j];
        if (sqrt(sum / 320.0) >= 200) voiced++;
    }
    free(samples);
    return voiced >= 8;
}

static int write_wav(const char *path, const int16_t *samples, size_t count) {
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    uint32_t data = (uint32_t)(count * 2);
    uint32_t riff = 36 + data;
    uint16_t channels = 1, bits = 16;
    uint32_t rate = 16000, byte_rate = 32000;
    uint16_t block = 2;
    fwrite("RIFF", 1, 4, file);
    fwrite(&riff, 4, 1, file);
    fwrite("WAVEfmt ", 1, 8, file);
    uint32_t fmt = 16;
    uint16_t pcm = 1;
    fwrite(&fmt, 4, 1, file);
    fwrite(&pcm, 2, 1, file);
    fwrite(&channels, 2, 1, file);
    fwrite(&rate, 4, 1, file);
    fwrite(&byte_rate, 4, 1, file);
    fwrite(&block, 2, 1, file);
    fwrite(&bits, 2, 1, file);
    fwrite("data", 1, 4, file);
    fwrite(&data, 4, 1, file);
    if (count) fwrite(samples, 2, count, file);
    fclose(file);
    return 0;
}

typedef struct record_job {
    voice_controller *app;
    char *path;
    char token[CTRL_TOKEN];
    target_snap *target;
    op_state *cancelled;
    char *previous;
    char mode[16];
    int retry;
    double deadline;
} record_job;

typedef struct record_recover {
    atomic_int refs;
    atomic_int recovered;
} record_recover;

typedef struct capture_owner {
    atomic_int refs;
    pthread_mutex_t mu;
    controller_capture *capture;
    int closed;
} capture_owner;

typedef struct drain_ctx {
    voice_controller *app;
    op_state *cancelled;
    char token[CTRL_TOKEN];
    target_snap *target;
    char *previous;
    char mode[16];
    char *spoken;
    record_recover *recovered;
} drain_ctx;

static record_recover *recover_new(void) {
    record_recover *state = calloc(1, sizeof *state);
    if (!state) return NULL;
    atomic_init(&state->refs, 1);
    atomic_init(&state->recovered, 0);
    return state;
}

static record_recover *recover_retain(record_recover *state) {
    if (state) atomic_fetch_add(&state->refs, 1);
    return state;
}

static void recover_release(record_recover *state) {
    if (state && atomic_fetch_sub(&state->refs, 1) == 1) free(state);
}

void *controller_capture_track(controller_capture *capture) {
    if (!capture) return NULL;
    capture_owner *owner = calloc(1, sizeof *owner);
    if (!owner) return NULL;
    atomic_init(&owner->refs, 1);
    pthread_mutex_init(&owner->mu, NULL);
    owner->capture = capture;
    return owner;
}

void controller_capture_publish(voice_controller *app, void *owner) {
    capture_owner *slot = owner;
    if (!app || !slot) return;
    atomic_fetch_add(&slot->refs, 1);
    app->capture_owner = slot;
    app->capture = slot->capture;
}

void *controller_capture_unpublish(voice_controller *app) {
    if (!app) return NULL;
    void *owner = app->capture_owner;
    app->capture_owner = NULL;
    app->capture = NULL;
    return owner;
}

static void owner_close(capture_owner *slot) {
    if (!slot) return;
    pthread_mutex_lock(&slot->mu);
    controller_capture *capture = slot->capture;
    int already = slot->closed || !capture || !capture->close;
    slot->closed = 1;
    pthread_mutex_unlock(&slot->mu);
    if (!already) capture->close(capture);
}

void controller_capture_release(void *owner) {
    capture_owner *slot = owner;
    if (!slot) return;
    owner_close(slot);
    if (atomic_fetch_sub(&slot->refs, 1) != 1) return;
    pthread_mutex_lock(&slot->mu);
    controller_capture *capture = slot->capture;
    slot->capture = NULL;
    pthread_mutex_unlock(&slot->mu);
    if (capture && capture->destroy) capture->destroy(capture);
    pthread_mutex_destroy(&slot->mu);
    free(slot);
}

static void recover_text(voice_controller *app, const char *text, const char *token, const target_data *target, op_state *cancelled, const char *previous, const char *mode, int drained, record_recover *recovered) {
    int locked = 0;
    for (int i = 0; i < 40 && !atomic_load(&app->attachments_closed); i++) {
        if (pthread_mutex_trylock(&app->state) == 0) {
            locked = 1;
            break;
        }
        usleep(50000);
    }
    if (!locked) return;
    if (atomic_load(&app->attachments_closed) || (recovered && atomic_load(&recovered->recovered)) || (drained && !(cancelled && cancelled->voice_target_lost))) {
        pthread_mutex_unlock(&app->state);
        return;
    }
    char cleaned[256 * 1024];
    if (dictation_text(text, cleaned, sizeof cleaned) != 0) cleaned[0] = 0;
    if (!cleaned[0]) {
        pthread_mutex_unlock(&app->state);
        return;
    }
    if (recovered) atomic_store(&recovered->recovered, 1);
    retained_dictation *kept = app->retained;
    int extend = previous && previous[0] && strcmp(mode, "append") == 0 && kept && strcmp(kept->source_token, token) == 0
        && strcmp(kept->text, previous) == 0 && cancelled && cancelled->recovery_revision == app->recovery_revision
        && cancelled->voice_target_lost;
    /* Append is an operation flag. A repeated phrase is still a new utterance. */
    if (previous && previous[0] && strcmp(mode, "append") == 0) {
        char *joined = malloc(strlen(previous) + strlen(cleaned) + 2);
        if (joined) {
            sprintf(joined, "%s\n%s", previous, cleaned);
            snprintf(cleaned, sizeof cleaned, "%s", joined);
            free(joined);
        }
    }
    if (extend) app->retained = NULL;
    if (!controller_retain(app, cleaned, token, target, cancelled)) {
        if (extend) app->retained = kept;
    } else if (extend && kept) {
        free(kept->text);
        free(kept);
    }
    pthread_mutex_unlock(&app->state);
}

static void drain_cb(const char *text, void *user) {
    drain_ctx *ctx = user;
    char piece[256 * 1024];
    if (dictation_text(text, piece, sizeof piece) != 0) piece[0] = 0;
    char *extra = ctx->spoken ? strdup(ctx->spoken) : strdup("");
    if (piece[0]) {
        char *joined = malloc(strlen(extra) + strlen(piece) + 2);
        if (joined) {
            sprintf(joined, "%s%s%s", extra, extra[0] ? "\n" : "", piece);
            free(extra);
            extra = joined;
        }
    }
    recover_text(ctx->app, extra, ctx->token, &ctx->target->data, ctx->cancelled, ctx->previous, ctx->mode, 1, ctx->recovered);
    free(extra);
}

static void drain_release(void *user) {
    drain_ctx *ctx = user;
    if (!ctx) return;
    controller_snap_release(ctx->target);
    controller_op_release(ctx->cancelled);
    recover_release(ctx->recovered);
    free(ctx->previous);
    free(ctx->spoken);
    free(ctx);
}

static int capture_failed(controller_capture *capture, int code) {
    const char *error = capture && capture->error ? capture->error(capture) : NULL;
    return (code != 0 && code != -SIGINT) || (error && error[0]);
}

static int await_lease(voice_controller *app, void *lease, op_state *cancelled) {
    if (!lease) return !atomic_load(&cancelled->cancelled);
    while (!atomic_load(&cancelled->cancelled)) {
        char err[CTRL_MSG] = {0};
        int wr = app->deps.engines.wait ? app->deps.engines.wait(lease, 100, err, sizeof err) : 0;
        if (wr == 0) return 1;
        if (wr == 1 && app->deps.engines.failed && app->deps.engines.failed(lease)) return 0;
        if (wr < 0) return 0;
    }
    return 0;
}

static void record_run(void *arg) {
    record_job *job = arg;
    voice_controller *app = job->app;
    controller_capture *capture = NULL;
    void *owner = NULL;
    void *lease = job->cancelled->engine_lease;
    record_recover *recovered = recover_new();
    char *spoken = strdup("");
    char held[256 * 1024];
    snprintf(held, sizeof held, "%s", job->previous && strcmp(job->mode, "append") == 0 ? job->previous : "");
    int staged_any = 0;
    char err[CTRL_MSG] = {0};
    int index = 0;
    if (!lease && app->deps.has_engines && app->deps.engines.acquire) {
        const char *stt = app->deps.audio.stt_backend ? app->deps.audio.stt_backend(app->deps.audio.user) : "whisper";
        if (strcmp(stt, "whisper") == 0) app->deps.engines.acquire(app->deps.engines.user, "stt", &lease, err, sizeof err);
    }
    if (app->deps.terminal.validate_target) {
        controller_target_view view;
        controller_view(&job->target->data, &view);
        if (app->deps.terminal.validate_target(app->deps.terminal.user, &view, err, sizeof err) != 0) goto fail;
    }
    if (atomic_load(&job->cancelled->cancelled)) goto done;
    if (!job->retry) {
        if (!app->deps.audio.start_capture) {
            snprintf(err, sizeof err, "Microphone capture failed");
            goto fail;
        }
        capture = app->deps.audio.start_capture(app->deps.audio.user, job->path, err, sizeof err);
        if (!capture) goto fail;
        owner = controller_capture_track(capture);
        if (!owner) {
            if (capture->destroy) capture->destroy(capture);
            capture = NULL;
            goto fail;
        }
        pthread_mutex_lock(&app->state);
        if (atomic_load(&job->cancelled->cancelled)) {
            pthread_mutex_unlock(&app->state);
            goto done;
        }
        controller_capture_publish(app, owner);
        pthread_mutex_unlock(&app->state);
        if (!capture->wait_ready || capture->wait_ready(capture, 5) != 0) {
            snprintf(err, sizeof err, "Microphone did not become ready");
            goto fail;
        }
        pthread_mutex_lock(&app->state);
        if (atomic_load(&job->cancelled->cancelled)) {
            pthread_mutex_unlock(&app->state);
            goto done;
        }
        app->record_started = controller_now(app);
        app->has_record_started = 1;
        snprintf(app->phase, sizeof app->phase, "recording");
        pthread_mutex_unlock(&app->state);
        if (app->deps.audio.cue) app->deps.audio.cue(app->deps.audio.user, 880, err, sizeof err);
    }
    int simple = !capture || !capture->drain_chunk || !capture->wait_progress;
    int finished = job->retry;
    for (;;) {
        if (atomic_load(&job->cancelled->cancelled)) break;
        const char *slice = job->path;
        char chunk_path[CTRL_PATH];
        int16_t *samples = NULL;
        size_t count = 0;
        if (!job->retry && !simple) {
            int drained = capture->drain_chunk(capture, &samples, &count);
            if (drained == 0 && samples && count) {
                index++;
                int named = snprintf(chunk_path, sizeof chunk_path, "%s-%d.wav", job->path, index);
                if (named < 0 || (size_t)named >= sizeof chunk_path || write_wav(chunk_path, samples, count) != 0) {
                    free(samples);
                    snprintf(err, sizeof err, "Microphone capture failed");
                    goto fail;
                }
                free(samples);
                slice = chunk_path;
            } else {
                free(samples);
                if (!finished) {
                    int code = 0;
                    int polled = capture->poll ? capture->poll(capture, &code) : 0;
                    if (polled == 1) {
                        if (capture->wait) capture->wait(capture, 1, &code);
                        if (capture_failed(capture, code)) {
                            snprintf(err, sizeof err, "%s", capture->error && capture->error(capture) ? capture->error(capture) : "Microphone capture failed");
                            goto fail;
                        }
                        finished = 1;
                        continue;
                    }
                    if (capture->wait_progress) capture->wait_progress(capture, 0.2);
                    continue;
                }
                break;
            }
        } else if (!job->retry && simple) {
            int code = 0;
            if (!capture->wait || capture->wait(capture, VOICE_RECORD_MAX_SECONDS, &code) != 0 || capture_failed(capture, code)) {
                snprintf(err, sizeof err, "%s", capture && capture->error && capture->error(capture) ? capture->error(capture) : "Microphone capture failed");
                goto fail;
            }
            finished = 1;
        }
        if (!has_audio_file(slice, err, sizeof err)) {
            if (err[0] && strcmp(err, "Expected 16 kHz mono PCM recording") == 0 && !job->retry) goto fail;
            err[0] = 0;
            if (job->retry || finished) break;
            continue;
        }
        if (!await_lease(app, lease, job->cancelled)) {
            recover_text(app, spoken, job->token, &job->target->data, job->cancelled, job->previous, job->mode, 0, recovered);
            goto done;
        }
        drain_ctx *drain = calloc(1, sizeof *drain);
        drain->app = app;
        drain->cancelled = controller_op_retain(job->cancelled);
        snprintf(drain->token, sizeof drain->token, "%s", job->token);
        drain->target = controller_snap_retain(job->target);
        drain->previous = job->previous ? strdup(job->previous) : NULL;
        snprintf(drain->mode, sizeof drain->mode, "%s", job->mode);
        drain->spoken = strdup(spoken);
        drain->recovered = recover_retain(recovered);
        char transcript[256 * 1024];
        transcript[0] = 0;
        int tr = -1;
        if (app->deps.audio.transcribe_owned)
            tr = app->deps.audio.transcribe_owned(app->deps.audio.user, slice, &job->cancelled->cancelled, drain_cb, drain_release, drain, transcript, sizeof transcript, err, sizeof err);
        else drain_release(drain);
        if (tr != 0) {
            pthread_mutex_lock(&app->state);
            if (!atomic_load(&job->cancelled->cancelled) && app->has_token && strcmp(job->token, app->token) == 0) {
                app->retry_lease = lease;
                lease = NULL;
                free(app->retry_path);
                app->retry_path = strdup(slice);
                controller_snap_release(app->retry_target);
                app->retry_target = controller_snap_retain(job->target);
                snprintf(app->retry_token, sizeof app->retry_token, "%s", job->token);
                app->retry_deadline = job->deadline > 0 ? job->deadline : controller_now(app) + VOICE_RETRY_SECONDS;
                free(app->retry_previous);
                app->retry_previous = strdup(held);
                snprintf(app->retry_mode, sizeof app->retry_mode, "%s", job->mode);
                app->has_retry = 1;
            }
            pthread_mutex_unlock(&app->state);
            goto fail;
        }
        char piece[256 * 1024];
        if (dictation_text(transcript, piece, sizeof piece) != 0) piece[0] = 0;
        if (!piece[0]) {
            if (job->retry || (finished && simple)) break;
            if (finished) break;
            continue;
        }
        char *next = malloc(strlen(spoken) + strlen(piece) + 2);
        sprintf(next, "%s%s%s", spoken, spoken[0] ? "\n" : "", piece);
        free(spoken);
        spoken = next;
        char outgoing[256 * 1024];
        if (staged_any) {
            size_t n = strlen(piece);
            if (n >= sizeof held) n = sizeof held - 1;
            memcpy(held, piece, n);
            held[n] = 0;
            memcpy(outgoing, held, n + 1);
        } else {
            size_t hlen = strlen(held), plen = strlen(piece);
            size_t need = hlen + (hlen ? 1 : 0) + plen;
            if (need >= sizeof held) need = sizeof held - 1;
            char *combined = malloc(need + 1);
            if (!combined) goto fail;
            size_t used = 0;
            if (hlen && used < need) {
                size_t take = hlen < need - used ? hlen : need - used;
                memcpy(combined + used, held, take);
                used += take;
                if (used < need) combined[used++] = '\n';
            }
            if (used < need) {
                size_t take = plen < need - used ? plen : need - used;
                memcpy(combined + used, piece, take);
                used += take;
            }
            combined[used] = 0;
            memcpy(held, combined, used + 1);
            memcpy(outgoing, combined, used + 1);
            free(combined);
        }
        int still = capture && capture->poll && capture->poll(capture, &(int){0}) == 0;
        int staged = controller_stage(app, outgoing, job->token, job->cancelled, !still, err, sizeof err);
        if (staged == CTRL_UNCERTAIN) {
            pthread_mutex_lock(&app->state);
            if (app->has_token && strcmp(job->token, app->token) == 0 && !atomic_load(&job->cancelled->cancelled)) {
                free(app->pending);
                app->pending = NULL;
                app->draft = 0;
            }
            pthread_mutex_unlock(&app->state);
            goto fail;
        }
        if (staged < 0) {
            pthread_mutex_lock(&app->state);
            if (atomic_load(&job->cancelled->cancelled) || !app->has_token || strcmp(job->token, app->token) != 0) {
                pthread_mutex_unlock(&app->state);
                recover_text(app, spoken, job->token, &job->target->data, job->cancelled, job->previous, job->mode, 0, recovered);
                goto done;
            }
            if (spoken[0] && !has_control_characters(spoken)) {
                free(app->pending);
                app->pending = strdup(held);
                if (!still) snprintf(app->phase, sizeof app->phase, "draft");
                pthread_mutex_unlock(&app->state);
                controller_notice(app, "Dictation retained", "Press Send when the agent is ready.", "orange");
                continue;
            }
            pthread_mutex_unlock(&app->state);
            goto fail;
        }
        if (staged > 0) {
            staged_any = 1;
            held[0] = 0;
        }
        if (job->retry || (simple && finished)) break;
        if (finished && !simple) break;
    }
    if (atomic_load(&job->cancelled->cancelled)) {
        recover_text(app, spoken, job->token, &job->target->data, job->cancelled, job->previous, job->mode, 0, recovered);
        goto done;
    }
    if (!spoken[0] && job->previous && job->previous[0]) {
        pthread_mutex_lock(&app->state);
        if (!atomic_load(&job->cancelled->cancelled) && app->has_token && strcmp(job->token, app->token) == 0) {
            free(app->pending);
            app->pending = strdup(job->previous);
            snprintf(app->phase, sizeof app->phase, "draft");
        }
        pthread_mutex_unlock(&app->state);
        controller_notice(app, "No new speech", "Previous dictation retained", "orange");
        goto done;
    }
    if (!staged_any && !app->pending && !spoken[0]) controller_notice(app, "No speech detected", "Nothing was inserted", "orange");
    goto done;
fail:
    pthread_mutex_lock(&app->state);
    if (!atomic_load(&job->cancelled->cancelled) && app->record_op == job->cancelled) {
        free(app->error);
        app->error = strdup(err[0] ? err : "Dictation stopped");
        snprintf(app->phase, sizeof app->phase, "error");
    }
    pthread_mutex_unlock(&app->state);
    if (err[0]) controller_notice(app, "Dictation stopped", err, "red");
done:
    pthread_mutex_lock(&app->state);
    if (owner && app->capture_owner == owner) controller_capture_release(controller_capture_unpublish(app));
    pthread_mutex_unlock(&app->state);
    controller_capture_release(owner);
    owner = NULL;
    capture = NULL;
    pthread_mutex_lock(&app->state);
    if (lease && lease != app->retry_lease && app->deps.engines.release) app->deps.engines.release(lease);
    if (app->has_active_retry && app->active_retry_op == job->cancelled) {
        free(app->active_retry_path);
        app->active_retry_path = NULL;
        controller_op_release(app->active_retry_op);
        app->active_retry_op = NULL;
        app->has_active_retry = 0;
    }
    int keep = app->has_retry && app->retry_path && strcmp(app->retry_path, job->path) == 0;
    if (!keep) unlink(job->path);
    for (int chunk = 1; chunk <= index; chunk++) {
        char extra[CTRL_PATH];
        int named = snprintf(extra, sizeof extra, "%s-%d.wav", job->path ? job->path : "", chunk);
        if (named < 0 || (size_t)named >= sizeof extra) continue;
        if (app->has_retry && app->retry_path && strcmp(extra, app->retry_path) == 0) continue;
        unlink(extra);
    }
    if (app->record_op == job->cancelled) {
        app->has_record_started = 0;
        if (controller_recording_active(app))
            snprintf(app->phase, sizeof app->phase, "%s", (app->draft || app->pending) ? "draft" : "idle");
    }
    pthread_mutex_unlock(&app->state);
    recover_release(recovered);
    controller_snap_release(job->target);
    controller_op_release(job->cancelled);
    free(job->previous);
    free(job->path);
    free(spoken);
    free(job);
}

int controller_record(voice_controller *app, const char *mode, char *err, size_t cap) {
    if (err && cap) err[0] = 0;
    controller_request_osd(app, NULL, 6, NULL);
    char *status = controller_status_json(app);
    pthread_mutex_lock(&app->state);
    if (app->retained) {
        pthread_mutex_unlock(&app->state);
        free(status);
        snprintf(err, cap, "Resolve retained dictation before recording again");
        controller_request_osd(app, err, 6, NULL);
        return CTRL_ERR;
    }
    controller_require_ready(app, app->has_token ? app->token : NULL, err, cap);
    if (err[0]) {
        pthread_mutex_unlock(&app->state);
        free(status);
        controller_request_osd(app, err, 6, NULL);
        return CTRL_ERR;
    }
    if (strcmp(app->phase, "starting") == 0) {
        pthread_mutex_unlock(&app->state);
        controller_stop(app, 1, 0);
        controller_notice(app, "Recording cancelled", "Microphone was starting", "orange");
        free(status);
        return CTRL_OK;
    }
    if (strcmp(app->phase, "recording") == 0) {
        snprintf(app->phase, sizeof app->phase, "stopping");
        void *owner = controller_capture_unpublish(app);
        pthread_mutex_unlock(&app->state);
        controller_capture_release(owner);
        free(status);
        return CTRL_OK;
    }
    if (strcmp(app->phase, "stopping") == 0) {
        pthread_mutex_unlock(&app->state);
        free(status);
        return CTRL_OK;
    }
    if (strcmp(app->phase, "transcribing") == 0) {
        pthread_mutex_unlock(&app->state);
        free(status);
        snprintf(err, cap, "Still transcribing; wait for the dictation to appear");
        controller_request_osd(app, err, 6, NULL);
        return CTRL_ERR;
    }
    if (!app->target) {
        pthread_mutex_unlock(&app->state);
        free(status);
        snprintf(err, cap, "Select a Pi voice session first");
        controller_request_osd(app, err, 6, NULL);
        return CTRL_ERR;
    }
    char origin[CTRL_TOKEN];
    snprintf(origin, sizeof origin, "%s", app->has_token ? app->token : "");
    target_data origin_identity = app->target->data;
    int origin_revision = app->recovery_revision;
    char *previous = app->pending ? strdup(app->pending) : NULL;
    pthread_mutex_unlock(&app->state);
    controller_stop(app, 0, 0);
    pthread_mutex_lock(&app->state);
    int switched = !app->has_token || strcmp(app->token, origin) != 0 || !app->target
        || app->recovery_revision != origin_revision + 1
        || !controller_same_attachment(&app->target->data, &origin_identity);
    if (switched) {
        pthread_mutex_unlock(&app->state);
        free(previous);
        free(status);
        snprintf(err, cap, "Voice session changed; discarded the old transcription");
        controller_request_osd(app, err, 6, NULL);
        return CTRL_ERR;
    }
    app->draft = 0;
    free(app->pending);
    app->pending = previous ? strdup(previous) : NULL;
    free(app->error);
    app->error = NULL;
    snprintf(app->phase, sizeof app->phase, "starting");
    free(app->recording_label);
    app->recording_label = strdup(app->target->data.pane);
    controller_op_release(app->record_op);
    app->record_op = controller_op_new(app);
    app->record_op->recovery_revision = app->recovery_revision;
    controller_op_release(app->input_op);
    app->input_op = controller_op_new(app);
    record_job *job = calloc(1, sizeof *job);
    char file[CTRL_PATH];
    snprintf(file, sizeof file, "%s/%llx.wav", app->runtime, (unsigned long long)app->record_op->generation);
    job->path = strdup(file);
    job->app = app;
    snprintf(job->token, sizeof job->token, "%s", origin);
    job->target = controller_snap(&origin_identity);
    job->cancelled = controller_op_retain(app->record_op);
    job->previous = previous;
    snprintf(job->mode, sizeof job->mode, "%s", mode ? mode : "append");
    job->deadline = controller_now(app) + VOICE_RETRY_SECONDS;
    pthread_mutex_unlock(&app->state);
    free(status);
    if (controller_submit(app, record_run, job) != 0) {
        snprintf(err, cap, "Voice is busy");
        controller_request_osd(app, err, 6, NULL);
        return CTRL_ERR;
    }
    return CTRL_OK;
}

int controller_retry(voice_controller *app, char *err, size_t cap) {
    if (err && cap) err[0] = 0;
    pthread_mutex_lock(&app->state);
    controller_expire_retry(app, 0);
    if (controller_recording_active(app)) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Finish the current recording first");
        return CTRL_ERR;
    }
    if (!app->has_retry || !app->retry_path) {
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "No recording available to retry");
        return CTRL_ERR;
    }
    if (!app->has_token || strcmp(app->retry_token, app->token) != 0) {
        unlink(app->retry_path);
        free(app->retry_path);
        app->retry_path = NULL;
        app->has_retry = 0;
        if (app->retry_lease && app->deps.engines.release) app->deps.engines.release(app->retry_lease);
        app->retry_lease = NULL;
        pthread_mutex_unlock(&app->state);
        snprintf(err, cap, "Voice session changed");
        return CTRL_ERR;
    }
    record_job *job = calloc(1, sizeof *job);
    job->app = app;
    job->path = app->retry_path;
    app->retry_path = NULL;
    app->has_retry = 0;
    snprintf(job->token, sizeof job->token, "%s", app->retry_token);
    job->target = app->retry_target;
    app->retry_target = NULL;
    job->previous = app->retry_previous;
    app->retry_previous = NULL;
    snprintf(job->mode, sizeof job->mode, "%s", app->retry_mode);
    job->retry = 1;
    job->deadline = app->retry_deadline;
    controller_op_release(app->record_op);
    app->record_op = controller_op_new(app);
    app->record_op->recovery_revision = app->recovery_revision;
    app->record_op->engine_lease = app->retry_lease;
    app->retry_lease = NULL;
    if (app->record_op->engine_lease && app->deps.engines.failed && app->deps.engines.failed(app->record_op->engine_lease)) {
        app->deps.engines.release(app->record_op->engine_lease);
        app->record_op->engine_lease = NULL;
    }
    job->cancelled = controller_op_retain(app->record_op);
    app->active_retry_path = strdup(job->path);
    app->active_retry_op = controller_op_retain(app->record_op);
    app->active_retry_deadline = job->deadline;
    app->has_active_retry = 1;
    snprintf(app->phase, sizeof app->phase, "transcribing");
    free(app->error);
    app->error = NULL;
    controller_op_release(app->input_op);
    app->input_op = controller_op_new(app);
    pthread_mutex_unlock(&app->state);
    if (controller_submit(app, record_run, job) != 0) {
        snprintf(err, cap, "Voice is busy");
        return CTRL_ERR;
    }
    return CTRL_OK;
}
