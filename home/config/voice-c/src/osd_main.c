#ifndef VOICE_OSD_NO_MAIN
#define osd_status_parse osd_status_parse_exe
#define osd_status_view osd_status_view_exe
#define osd_status_free osd_status_free_exe
#define osd_status_contains osd_status_contains_exe
#define osd_notice_view osd_notice_view_exe
#define osd_unavailable osd_unavailable_exe
#define osd_apply_sample osd_apply_sample_exe
#define osd_apply_stale osd_apply_stale_exe
#define osd_monitors_parse osd_monitors_parse_exe
#define osd_monitors_free osd_monitors_free_exe
#define osd_theme_paths osd_theme_paths_exe
#define osd_accessible_label osd_accessible_label_exe
#define osd_frame_continues osd_frame_continues_exe
#endif
#include "osd_main.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

struct OsdStatusDoc {
    VoiceStatus status;
    VoiceSession *sessions;
    char **owned;
    size_t owned_count;
};

static char *own(OsdStatusDoc *doc, const char *text) {
    char *copy;
    char **next;
    size_t n;
    if (!text) return NULL;
    n = strlen(text) + 1;
    copy = malloc(n);
    next = realloc(doc->owned, (doc->owned_count + 1) * sizeof *next);
    if (!copy || !next) {
        free(copy);
        free(next);
        return NULL;
    }
    memcpy(copy, text, n);
    doc->owned = next;
    doc->owned[doc->owned_count++] = copy;
    return copy;
}

static const char *json_str(yyjson_val *value) {
    return yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static int json_flag(yyjson_val *value) {
    if (!value || yyjson_is_null(value) || yyjson_is_false(value)) return 0;
    if (yyjson_is_true(value)) return 1;
    if (yyjson_is_str(value)) return json_str(value)[0] != 0;
    if (yyjson_is_num(value)) return yyjson_get_num(value) != 0.0;
    return 0;
}

OsdStatusDoc *osd_status_parse(const char *json) {
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *sessions;
    yyjson_val *mic;
    OsdStatusDoc *status;
    size_t idx, max, count = 0;
    yyjson_val *row;
    if (!json) return NULL;
    doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return NULL;
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root) || !yyjson_is_true(yyjson_obj_get(root, "ok"))) {
        yyjson_doc_free(doc);
        return NULL;
    }
    status = calloc(1, sizeof *status);
    if (!status) {
        yyjson_doc_free(doc);
        return NULL;
    }
    status->status.phase = own(status, json_str(yyjson_obj_get(root, "phase")));
    status->status.osd = json_flag(yyjson_obj_get(root, "osd"));
    status->status.pane = own(status, json_str(yyjson_obj_get(root, "pane")));
    status->status.recording_label = own(status, json_str(yyjson_obj_get(root, "recording_label")));
    status->status.session_label = own(status, json_str(yyjson_obj_get(root, "session_label")));
    status->status.osd_message = own(status, json_str(yyjson_obj_get(root, "osd_message")));
    status->status.osd_tone = own(status, json_str(yyjson_obj_get(root, "osd_tone")));
    status->status.error = own(status, json_str(yyjson_obj_get(root, "error")));
    status->status.agent_state = own(status, json_str(yyjson_obj_get(root, "agent_state")));
    status->status.connection_state = own(status, json_str(yyjson_obj_get(root, "connection_state")));
    status->status.models = own(status, json_str(yyjson_obj_get(root, "models")));
    status->status.model_error = own(status, json_str(yyjson_obj_get(root, "model_error")));
    status->status.retained_source = own(status, json_str(yyjson_obj_get(root, "retained_source")));
    status->status.queued = json_flag(yyjson_obj_get(root, "queued"));
    status->status.pending = json_flag(yyjson_obj_get(root, "pending"));
    status->status.draft = json_flag(yyjson_obj_get(root, "draft"));
    status->status.draft_edited = json_flag(yyjson_obj_get(root, "draft_edited"));
    status->status.retained = json_flag(yyjson_obj_get(root, "retained"));
    status->status.speaking = json_flag(yyjson_obj_get(root, "speaking"));
    status->status.audible = json_flag(yyjson_obj_get(root, "audible"));
    status->status.rebind_needed = json_flag(yyjson_obj_get(root, "rebind_needed"));
    if (yyjson_is_num(yyjson_obj_get(root, "recording_seconds"))) {
        status->status.has_recording_seconds = 1;
        status->status.recording_seconds = yyjson_get_num(yyjson_obj_get(root, "recording_seconds"));
    } else if (yyjson_obj_get(root, "recording_seconds")) status->status.recording_seconds_invalid = 1;
    if (yyjson_is_num(yyjson_obj_get(root, "input_level"))) {
        status->status.has_input_level = 1;
        status->status.input_level = yyjson_get_num(yyjson_obj_get(root, "input_level"));
    } else if (yyjson_obj_get(root, "input_level")) status->status.input_level_invalid = 1;
    mic = yyjson_obj_get(root, "microphone");
    if (yyjson_is_obj(mic)) {
        status->status.has_microphone = 1;
        status->status.mic_muted = json_flag(yyjson_obj_get(mic, "muted"));
        status->status.mic_clipping = json_flag(yyjson_obj_get(mic, "clipping"));
    }
    sessions = yyjson_obj_get(root, "sessions");
    if (yyjson_is_arr(sessions)) {
        yyjson_arr_foreach(sessions, idx, max, row) if (yyjson_is_obj(row)) count++;
        status->sessions = calloc(count ? count : 1, sizeof *status->sessions);
        if (!status->sessions && count) {
            osd_status_free(status);
            yyjson_doc_free(doc);
            return NULL;
        }
        count = 0;
        yyjson_arr_foreach(sessions, idx, max, row) {
            if (!yyjson_is_obj(row)) continue;
            status->sessions[count].selected = json_flag(yyjson_obj_get(row, "selected"));
            status->sessions[count].full_label = own(status, json_str(yyjson_obj_get(row, "full_label")));
            status->sessions[count].label = own(status, json_str(yyjson_obj_get(row, "label")));
            count++;
        }
        status->status.sessions = status->sessions;
        status->status.session_count = count;
    }
    yyjson_doc_free(doc);
    return status;
}

const VoiceStatus *osd_status_view(const OsdStatusDoc *doc) {
    return doc ? &doc->status : NULL;
}

void osd_status_free(OsdStatusDoc *doc) {
    size_t i;
    if (!doc) return;
    for (i = 0; i < doc->owned_count; i++) free(doc->owned[i]);
    free(doc->owned);
    free(doc->sessions);
    free(doc);
}

int osd_status_contains(const OsdStatusDoc *doc, const char *needle) {
    size_t i;
    const VoiceStatus *status;
    if (!doc || !needle) return 0;
    status = &doc->status;
    if ((status->phase && strstr(status->phase, needle))
        || (status->pane && strstr(status->pane, needle))
        || (status->recording_label && strstr(status->recording_label, needle))
        || (status->session_label && strstr(status->session_label, needle))
        || (status->osd_message && strstr(status->osd_message, needle))
        || (status->error && strstr(status->error, needle))
        || (status->model_error && strstr(status->model_error, needle))
        || (status->retained_source && strstr(status->retained_source, needle))) return 1;
    for (i = 0; i < status->session_count; i++) {
        if ((status->sessions[i].label && strstr(status->sessions[i].label, needle))
            || (status->sessions[i].full_label && strstr(status->sessions[i].full_label, needle))) return 1;
    }
    return 0;
}

static int tone_ok(const char *tone) {
    return tone && (strcmp(tone, "red") == 0 || strcmp(tone, "yellow") == 0 || strcmp(tone, "green") == 0
        || strcmp(tone, "orange") == 0 || strcmp(tone, "teal") == 0 || strcmp(tone, "accent") == 0
        || strcmp(tone, "muted") == 0);
}

int osd_notice_view(const char *json, double now_unix, OsdView *out) {
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *until;
    yyjson_val *message;
    const char *text;
    const char *tone;
    VoiceStatus status;
    char *cursor;
    if (!out || !json) return 0;
    doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return 0;
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return 0;
    }
    until = yyjson_obj_get(root, "until");
    if (!yyjson_is_num(until) || yyjson_get_num(until) <= now_unix) {
        yyjson_doc_free(doc);
        return 0;
    }
    message = yyjson_obj_get(root, "message");
    text = json_str(message);
    if (!text) {
        yyjson_doc_free(doc);
        return 0;
    }
    cursor = (char *)text;
    while (*cursor && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')) cursor++;
    if (!cursor[0]) {
        yyjson_doc_free(doc);
        return 0;
    }
    tone = json_str(yyjson_obj_get(root, "tone"));
    memset(&status, 0, sizeof status);
    status.osd = 1;
    status.osd_message = text;
    status.osd_tone = tone_ok(tone) ? tone : "red";
    osd_view(&status, out);
    yyjson_doc_free(doc);
    return out->title[0] != 0;
}

void osd_unavailable(PillView *out) {
    VoiceStatus status;
    OsdView view;
    if (!out) return;
    memset(&status, 0, sizeof status);
    status.phase = "error";
    status.error = "Voice connection lost";
    status.session_label = "Microphone state unknown";
    osd_view(&status, &view);
    pill_view(&view, out);
}

void osd_apply_sample(int have_status, const VoiceStatus *status, int *was_recording,
    WarningFilter *filter, double now_mono, const char *notice_json, double now_unix, PillView *out) {
    OsdView card;
    OsdView filtered;
    WarningFilter local;
    if (!filter) {
        warning_filter_init(&local);
        filter = &local;
    }
    if (!have_status && was_recording && *was_recording) {
        OsdView empty;
        memset(&empty, 0, sizeof empty);
        warning_filter_apply(filter, &empty, now_mono, &filtered);
        osd_unavailable(out);
        return;
    }
    osd_view(have_status ? status : NULL, &card);
    if (was_recording) *was_recording = card.recording;
    if (!card.visible && notice_json) {
        OsdView notice;
        if (osd_notice_view(notice_json, now_unix, &notice)) card = notice;
    }
    warning_filter_apply(filter, &card, now_mono, &filtered);
    pill_view(&filtered, out);
}

int osd_apply_stale(const PillView *current, double sampled_mono, double now_mono, PillView *out) {
    OsdView empty;
    if (!current || !out || now_mono - sampled_mono <= 2.0) return 0;
    if (current->recording) osd_unavailable(out);
    else {
        osd_view(NULL, &empty);
        pill_view(&empty, out);
    }
    return 1;
}

int osd_monitors_parse(const char *json, VoiceMonitor **monitors, char ***names, size_t *count) {
    yyjson_doc *doc;
    yyjson_val *root;
    size_t idx, max, n = 0;
    yyjson_val *row;
    if (!monitors || !names || !count) return -1;
    *monitors = NULL;
    *names = NULL;
    *count = 0;
    if (!json) return -1;
    doc = yyjson_read(json, strlen(json), 0);
    if (!doc) return -1;
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return -1;
    }
    n = yyjson_arr_size(root);
    *monitors = calloc(n ? n : 1, sizeof **monitors);
    *names = calloc(n ? n : 1, sizeof **names);
    if (!*monitors || !*names) {
        free(*monitors);
        free(*names);
        *monitors = NULL;
        *names = NULL;
        yyjson_doc_free(doc);
        return -1;
    }
    n = 0;
    yyjson_arr_foreach(root, idx, max, row) {
        const char *name;
        if (!yyjson_is_obj(row)) continue;
        name = json_str(yyjson_obj_get(row, "name"));
        if (!name || !name[0]) continue;
        (*names)[n] = malloc(strlen(name) + 1);
        if (!(*names)[n]) {
            osd_monitors_free(*monitors, *names, n);
            *monitors = NULL;
            *names = NULL;
            *count = 0;
            yyjson_doc_free(doc);
            return -1;
        }
        memcpy((*names)[n], name, strlen(name) + 1);
        (*monitors)[n].name = (*names)[n];
        (*monitors)[n].focused = yyjson_is_true(yyjson_obj_get(row, "focused")) ? 1 : 0;
        n++;
    }
    *count = n;
    yyjson_doc_free(doc);
    return 0;
}

void osd_monitors_free(VoiceMonitor *monitors, char **names, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) free(names ? names[i] : NULL);
    free(names);
    free(monitors);
}

void osd_theme_paths(const char *xdg_state, const char *home, char *shell, size_t shell_cap, char *waybar, size_t waybar_cap) {
    const char *slash;
    if (!shell || !waybar || !shell_cap || !waybar_cap) return;
    shell[0] = 0;
    waybar[0] = 0;
    if (xdg_state && xdg_state[0]) snprintf(shell, shell_cap, "%s/theme-menu/active/shell.toml", xdg_state);
    else snprintf(shell, shell_cap, "%s/.local/state/theme-menu/active/shell.toml", home ? home : "");
    snprintf(waybar, waybar_cap, "%s", shell);
    slash = strrchr(waybar, '/');
    if (!slash || (size_t)(slash - waybar) + strlen("/waybar.css") + 1 > waybar_cap) return;
    snprintf((char *)slash, waybar_cap - (size_t)(slash - waybar), "/waybar.css");
}

void osd_accessible_label(const PillView *view, char *out, size_t cap) {
    const char *title;
    if (!out || !cap) return;
    if (!view || !view->visible) {
        snprintf(out, cap, "Voice idle");
        return;
    }
    /* pill_view uses "Voice" when the card title is empty. Keep that name audible. */
    title = view->title[0] ? view->title : "Voice";
    snprintf(out, cap, "%s. %s", title, view->detail);
}

int osd_frame_continues(const PillView *view, const PillMotion *motion, int reduced, int keep) {
    int settling;
    int changing;
    if (!view || !motion) return 0;
    settling = fabs(motion->width - view->width) > 0.1
        || fabs(motion->height - view->height) > 0.1
        || fabs(motion->opacity - (view->visible ? 1.0 : 0.0)) > 0.005;
    changing = (view->recording && !view->muted) || (view->working && !reduced);
    return keep && (settling || changing || !view->visible);
}

#ifndef VOICE_OSD_NO_MAIN
#include "ipc.h"
#include "runtime_adapters.h"

#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int started;
    int stop;
    PillView view;
    char connector[128];
    int has_connector;
    double sampled;
    char socket_path[576];
    char notice_path[640];
} OsdShared;

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *canvas;
    PillMotion motion;
    PillView view;
    int animating;
    char accessible[512];
    char connector[128];
    OsdShared *shared;
    VoiceColours colours;
} OsdWindow;

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static double unix_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static char *read_small(const char *path) {
    FILE *file = fopen(path, "r");
    char *buf;
    long len;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    len = ftell(file);
    if (len < 0 || len > 1024 * 1024) { fclose(file); return NULL; }
    rewind(file);
    buf = malloc((size_t)len + 1);
    if (!buf) { fclose(file); return NULL; }
    if (fread(buf, 1, (size_t)len, file) != (size_t)len) { free(buf); fclose(file); return NULL; }
    buf[len] = 0;
    fclose(file);
    return buf;
}

static void *poll_thread(void *arg) {
    OsdShared *shared = arg;
    WarningFilter warnings;
    int was_recording = 0;
    double next_monitor = 0;
    warning_filter_init(&warnings);
    while (1) {
        yyjson_mut_doc *mut;
        yyjson_doc *request = NULL;
        yyjson_doc *response = NULL;
        int sent = 0;
        int have = 0;
        OsdStatusDoc *parsed = NULL;
        char *notice = NULL;
        char *written = NULL;
        PillView pill;
        char connector[128];
        int has_connector = 0;
        ipc_unix_request call;
        char err[256];
        pthread_mutex_lock(&shared->lock);
        if (shared->stop) { pthread_mutex_unlock(&shared->lock); break; }
        pthread_mutex_unlock(&shared->lock);
        mut = yyjson_mut_doc_new(NULL);
        if (mut) {
            yyjson_mut_val *root = yyjson_mut_obj(mut);
            yyjson_mut_doc_set_root(mut, root);
            yyjson_mut_obj_add_str(mut, root, "action", "status");
            request = yyjson_mut_doc_imut_copy(mut, NULL);
            yyjson_mut_doc_free(mut);
        }
        memset(&call, 0, sizeof call);
        call.socket_path = shared->socket_path;
        call.request = request;
        call.deadline_ms = 400;
        call.max_response = 1024 * 1024;
        if (request && ipc_unix_json(&call, &sent, &response, err, sizeof err) == IPC_OK) {
            written = yyjson_write(response, 0, NULL);
            parsed = osd_status_parse(written);
            have = parsed != NULL;
        }
        yyjson_doc_free(request);
        yyjson_doc_free(response);
        free(written);
        if (!have || (parsed && !osd_status_view(parsed)->osd && osd_status_view(parsed)->phase
            && strcmp(osd_status_view(parsed)->phase, "idle") == 0)) {
            /* Notice is applied after visibility is known. Read it every sample. */
        }
        notice = read_small(shared->notice_path);
        osd_apply_sample(have, parsed ? osd_status_view(parsed) : NULL, &was_recording, &warnings,
            mono_now(), notice, unix_now(), &pill);
        osd_status_free(parsed);
        free(notice);
        if (mono_now() >= next_monitor) {
            const char *argv[] = {"hyprctl", "monitors", "-j", NULL};
            ipc_env_override env = {"LD_PRELOAD", ""};
            ipc_process_request proc;
            ipc_process_result result;
            memset(&proc, 0, sizeof proc);
            proc.argv = (const char *const *)argv;
            proc.argc = 3;
            proc.capture_stdout = 1;
            proc.deadline_ms = 1000;
            proc.env = &env;
            proc.env_count = 1;
            if (ipc_process_run(&proc, &result, err, sizeof err) == IPC_OK && result.exit_code == 0 && result.stdout_bytes) {
                char *json = calloc(result.stdout_len + 1, 1);
                VoiceMonitor *monitors = NULL;
                char **names = NULL;
                size_t count = 0;
                const char *focused;
                if (json) memcpy(json, result.stdout_bytes, result.stdout_len);
                if (json && osd_monitors_parse(json, &monitors, &names, &count) == 0) {
                    focused = focused_connector(monitors, count);
                    if (focused) {
                        snprintf(connector, sizeof connector, "%s", focused);
                        has_connector = 1;
                    }
                    osd_monitors_free(monitors, names, count);
                }
                free(json);
            }
            ipc_process_result_free(&result);
            next_monitor = mono_now() + 1.0;
        }
        pthread_mutex_lock(&shared->lock);
        shared->view = pill;
        shared->sampled = mono_now();
        if (has_connector) {
            snprintf(shared->connector, sizeof shared->connector, "%s", connector);
            shared->has_connector = 1;
        }
        pthread_mutex_unlock(&shared->lock);
        {
            struct timespec pause = {0, 80000000};
            nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

static int reduced_motion(void) {
    gboolean enabled = TRUE;
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) return 0;
    g_object_get(settings, "gtk-enable-animations", &enabled, NULL);
    return !enabled;
}

static void clear_input(GtkWidget *window, gpointer user) {
    GdkSurface *surface;
    cairo_region_t *empty;
    (void)user;
    surface = gtk_native_get_surface(GTK_NATIVE(window));
    empty = cairo_region_create();
    if (surface && empty) gdk_surface_set_input_region(surface, empty);
    if (empty) cairo_region_destroy(empty);
}

static void draw_pill(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user) {
    OsdWindow *osd = user;
    (void)area;
    (void)width;
    (void)height;
    pill_paint(cr, &osd->view, &osd->motion, &osd->colours, mono_now(), reduced_motion());
}

static gboolean on_frame(GtkWidget *widget, GdkFrameClock *clock, gpointer user) {
    OsdWindow *osd = user;
    int keep;
    (void)widget;
    (void)clock;
    keep = pill_motion_step(&osd->motion, &osd->view, mono_now(), reduced_motion());
    gtk_widget_queue_draw(osd->canvas);
    if (!keep) gtk_widget_set_visible(osd->window, FALSE);
    osd->animating = osd_frame_continues(&osd->view, &osd->motion, reduced_motion(), keep);
    return osd->animating;
}

static gboolean refresh_window(gpointer user) {
    OsdWindow *osd = user;
    PillView view;
    double sampled;
    char connector[128];
    int has_connector = 0;
    char label[512];
    int changed;
    pthread_mutex_lock(&osd->shared->lock);
    view = osd->shared->view;
    sampled = osd->shared->sampled;
    if (osd->shared->has_connector) {
        snprintf(connector, sizeof connector, "%s", osd->shared->connector);
        has_connector = 1;
    }
    pthread_mutex_unlock(&osd->shared->lock);
    if (osd_apply_stale(&view, sampled, mono_now(), &view)) {
        /* A stale recording is unknown, not a closed microphone. */
    }
    changed = memcmp(&view, &osd->view, sizeof view) != 0;
    osd->view = view;
    osd_accessible_label(&view, label, sizeof label);
    if (strcmp(label, osd->accessible) != 0) {
        gtk_accessible_update_property(GTK_ACCESSIBLE(osd->canvas), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
        snprintf(osd->accessible, sizeof osd->accessible, "%s", label);
    }
    if (has_connector && strcmp(connector, osd->connector) != 0) {
        GdkDisplay *display = gdk_display_get_default();
        GListModel *monitors = display ? gdk_display_get_monitors(display) : NULL;
        guint i, n = monitors ? g_list_model_get_n_items(monitors) : 0;
        for (i = 0; i < n; i++) {
            GdkMonitor *monitor = g_list_model_get_item(monitors, i);
            const char *name = monitor ? gdk_monitor_get_connector(monitor) : NULL;
            if (name && strcmp(name, connector) == 0) {
                gtk_layer_set_monitor(GTK_WINDOW(osd->window), monitor);
                snprintf(osd->connector, sizeof osd->connector, "%s", connector);
                g_object_unref(monitor);
                break;
            }
            if (monitor) g_object_unref(monitor);
        }
    }
    if (view.visible && !gtk_widget_get_visible(osd->window)) {
        gtk_widget_set_visible(osd->window, TRUE);
        clear_input(osd->window, NULL);
    }
    if (changed && !osd->animating && (view.visible || gtk_widget_get_visible(osd->window))) {
        osd->animating = TRUE;
        gtk_widget_add_tick_callback(osd->canvas, on_frame, osd, NULL);
    }
    return G_SOURCE_CONTINUE;
}

static void activate(GtkApplication *app, gpointer user) {
    OsdWindow *osd = user;
    GtkCssProvider *css;
    osd->app = app;
    osd->window = gtk_application_window_new(app);
    gtk_widget_set_name(osd->window, "voice-pill");
    gtk_window_set_decorated(GTK_WINDOW(osd->window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(osd->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(osd->window), PILL_SURFACE_WIDTH, PILL_SURFACE_HEIGHT);
    gtk_widget_set_can_focus(osd->window, FALSE);
    gtk_widget_set_focus_on_click(osd->window, FALSE);
    gtk_layer_init_for_window(GTK_WINDOW(osd->window));
    gtk_layer_set_layer(GTK_WINDOW(osd->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(osd->window), "pi-voice-osd");
    gtk_layer_set_keyboard_mode(GTK_WINDOW(osd->window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(osd->window), 0);
    gtk_layer_set_anchor(GTK_WINDOW(osd->window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(osd->window), GTK_LAYER_SHELL_EDGE_TOP, 18);
    osd->canvas = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(osd->canvas), PILL_SURFACE_WIDTH);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(osd->canvas), PILL_SURFACE_HEIGHT);
    gtk_widget_set_can_target(osd->canvas, FALSE);
    gtk_widget_set_can_focus(osd->canvas, FALSE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(osd->canvas), draw_pill, osd, NULL);
    gtk_window_set_child(GTK_WINDOW(osd->window), osd->canvas);
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window#voice-pill, window#voice-pill * {"
        " background: transparent; background-color: transparent;"
        " box-shadow: none; border: none; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    g_object_unref(css);
    g_signal_connect(osd->window, "map", G_CALLBACK(clear_input), NULL);
    gtk_widget_set_visible(osd->window, FALSE);
    g_timeout_add(80, refresh_window, osd);
    pthread_create(&osd->shared->thread, NULL, poll_thread, osd->shared);
    osd->shared->started = 1;
}

int main(int argc, char **argv) {
    char runtime[512];
    char err[256];
    char shell[512];
    char waybar[512];
    char *shell_text = NULL;
    char *waybar_text = NULL;
    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    OsdShared shared;
    OsdWindow osd;
    int status;
    char *app_argv[] = {"pi-voice-osd", NULL};
    (void)argc;
    (void)argv;
    if (!getenv("WAYLAND_DISPLAY") || !getenv("WAYLAND_DISPLAY")[0]) {
        fprintf(stderr, "pi-voice-osd: no Wayland display\n");
        return 0;
    }
    if (voice_runtime_dir_resolve(runtime, sizeof runtime, err, sizeof err)) {
        fprintf(stderr, "pi-voice-osd: %s\n", err[0] ? err : "runtime directory is unavailable");
        return 1;
    }
    memset(&shared, 0, sizeof shared);
    memset(&osd, 0, sizeof osd);
    pthread_mutex_init(&shared.lock, NULL);
    snprintf(shared.socket_path, sizeof shared.socket_path, "%s/control.sock", runtime);
    snprintf(shared.notice_path, sizeof shared.notice_path, "%s/osd-notice.json", runtime);
    shared.sampled = mono_now();
    pill_view(NULL, &shared.view);
    osd.shared = &shared;
    osd.view = shared.view;
    pill_motion_init(&osd.motion);
    osd_theme_paths(state, home, shell, sizeof shell, waybar, sizeof waybar);
    shell_text = read_small(shell);
    waybar_text = read_small(waybar);
    resolved_colours(shell_text ? shell_text : "", waybar_text ? waybar_text : "", &osd.colours);
    free(shell_text);
    free(waybar_text);
    osd.app = gtk_application_new("uk.hues.pi-voice-osd", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(osd.app, "activate", G_CALLBACK(activate), &osd);
    status = g_application_run(G_APPLICATION(osd.app), 1, app_argv);
    pthread_mutex_lock(&shared.lock);
    shared.stop = 1;
    pthread_mutex_unlock(&shared.lock);
    if (shared.started) pthread_join(shared.thread, NULL);
    pthread_mutex_destroy(&shared.lock);
    g_object_unref(osd.app);
    return status;
}
#endif
