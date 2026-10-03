#include "pill.h"

#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

static VoiceStatus blank(void) {
    VoiceStatus status;
    memset(&status, 0, sizeof status);
    return status;
}

static PillView project(VoiceStatus status) {
    OsdView osd;
    PillView pill;
    osd_view(&status, &osd);
    pill_view(&osd, &pill);
    return pill;
}

static void join_pill(const PillView *view, char *out, size_t cap) {
    snprintf(out, cap, "%d %d %.6f %s %s %s %s %d %d %d %d %.3f %.3f %s",
        view->visible, view->recording, view->level, view->title, view->detail, view->tone,
        view->timer, view->warning, view->muted, view->working, view->expanded,
        view->width, view->height, view->phase);
}

static int levels_equal(const double *levels, double value) {
    for (int i = 0; i < PILL_BARS; i++) {
        if (levels[i] != value) return 0;
    }
    return 1;
}

static void read_pixel(cairo_surface_t *surface, int x, int y, double *a, double *r, double *g, double *b) {
    int stride = cairo_image_surface_get_stride(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    unsigned int pixel;
    memcpy(&pixel, data + (size_t)y * (size_t)stride + (size_t)x * 4, 4);
    *a = ((pixel >> 24) & 255) / 255.0;
    *r = ((pixel >> 16) & 255) / 255.0;
    *g = ((pixel >> 8) & 255) / 255.0;
    *b = (pixel & 255) / 255.0;
    if (*a > 0) {
        *r /= *a;
        *g /= *a;
        *b /= *a;
    }
}

static int paint_geometry(void) {
    VoiceStatus status = blank();
    PillView view;
    PillMotion motion;
    VoiceColours colours;
    cairo_surface_t *surface;
    cairo_t *cr;
    double a, r, g, b;
    double x;
    double y;
    double w;
    int bar_x;
    int light_x;
    int cy;
    status.phase = "recording";
    status.has_recording_seconds = 1;
    status.recording_seconds = 10;
    status.has_input_level = 1;
    status.input_level = 1;
    view = project(status);
    pill_motion_init(&motion);
    if (!pill_motion_step(&motion, &view, 1, 1)) return 0;
    if (motion.width != 208 || motion.height != 44 || motion.opacity != 1) return 0;
    if (motion.levels[PILL_BARS - 1] != 1 || motion.levels[0] != 0) return 0;
    resolved_colours(NULL, NULL, &colours);
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PILL_SURFACE_WIDTH, PILL_SURFACE_HEIGHT);
    cr = cairo_create(surface);
    pill_paint(cr, &view, &motion, &colours, 1, 1);
    cairo_surface_flush(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return 0;
    }
    w = motion.width;
    x = (PILL_SURFACE_WIDTH - w) / 2.0;
    y = 12;
    read_pixel(surface, 2, 2, &a, &r, &g, &b);
    if (a != 0) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return 0;
    }
    read_pixel(surface, (int)(x + w / 2.0), (int)(y - 3), &a, &r, &g, &b);
    if (a < 0.01 || a > 0.06) {
        fprintf(stderr, "shadow alpha %.4f\n", a);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return 0;
    }
    read_pixel(surface, (int)(x + 54), (int)(y + 6), &a, &r, &g, &b);
    if (a < 0.9 || fabs(r - 0.047) > 0.02 || fabs(g - 0.051) > 0.02 || fabs(b - 0.063) > 0.02) {
        fprintf(stderr, "capsule %.3f %.3f %.3f a=%.3f\n", r, g, b, a);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return 0;
    }
    light_x = (int)(x + 22);
    cy = (int)(y + 22);
    read_pixel(surface, light_x, cy, &a, &r, &g, &b);
    if (a < 0.9 || r < 0.7 || g > 0.35 || b > 0.4) {
        fprintf(stderr, "capture light %.3f %.3f %.3f a=%.3f\n", r, g, b, a);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return 0;
    }
    bar_x = (int)(x + 43 + 16 * 4.5 + 1);
    read_pixel(surface, bar_x, cy, &a, &r, &g, &b);
    if (a < 0.9 || r < 0.85 || g < 0.85 || b < 0.85) {
        fprintf(stderr, "history bar %.3f %.3f %.3f a=%.3f at %d,%d\n", r, g, b, a, bar_x, cy);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return 0;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    {
        PillView branches[4];
        const char *long_title = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        memset(branches, 0, sizeof branches);
        branches[0] = view;
        branches[0].working = 1;
        branches[0].recording = 0;
        strcpy(branches[0].tone, "yellow");
        strcpy(branches[0].title, "Transcribing");
        branches[0].expanded = 0;
        branches[0].width = 208;
        branches[0].height = 44;
        branches[1] = view;
        branches[1].warning = 1;
        branches[1].recording = 1;
        strcpy(branches[1].tone, "orange");
        strcpy(branches[1].title, "Input too loud");
        strcpy(branches[1].detail, long_title);
        branches[1].expanded = 1;
        branches[1].width = 352;
        branches[1].height = 76;
        branches[2] = view;
        branches[2].recording = 0;
        branches[2].warning = 0;
        strcpy(branches[2].tone, "green");
        strcpy(branches[2].title, "Ready to send");
        branches[2].expanded = 1;
        branches[2].width = 352;
        branches[2].height = 76;
        branches[3] = view;
        branches[3].recording = 0;
        branches[3].warning = 0;
        strcpy(branches[3].tone, "red");
        strcpy(branches[3].title, long_title);
        branches[3].expanded = 1;
        branches[3].width = 352;
        branches[3].height = 76;
        for (int i = 0; i < 4; i++) {
            PillMotion snapped;
            pill_motion_init(&snapped);
            pill_motion_step(&snapped, &branches[i], 1, 1);
            surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, PILL_SURFACE_WIDTH, PILL_SURFACE_HEIGHT);
            cr = cairo_create(surface);
            pill_paint(cr, &branches[i], &snapped, &colours, 2.5, i == 0);
            cairo_surface_flush(surface);
            if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
                cairo_destroy(cr);
                cairo_surface_destroy(surface);
                return 0;
            }
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
        }
    }
    return 1;
}

int test_pill(void) {
    char rendered[1024];
    failures = 0;

    {
        VoiceStatus status = blank();
        status.phase = "idle";
        PillView view = project(status);
        join_pill(&view, rendered, sizeof rendered);
        if (view.visible || strstr(rendered, "SECRET")) fail("idle hidden and private fields never projected");
    }

    {
        VoiceStatus start_status = blank();
        VoiceStatus settled_status = blank();
        PillView start;
        PillView settled;
        start_status.phase = "recording";
        start_status.has_recording_seconds = 1;
        start_status.recording_seconds = 1;
        start_status.recording_label = "pi \xc2\xb7 Notes";
        settled_status = start_status;
        settled_status.recording_seconds = 65;
        start = project(start_status);
        settled = project(settled_status);
        if (!start.expanded || settled.expanded || strcmp(settled.timer, "01:05") != 0
            || strcmp(start.detail, "Notes") != 0 || !(settled.width < start.width)) {
            fail("recording reveals destination then compacts");
        }
    }

    {
        VoiceStatus status = blank();
        PillView view;
        PillMotion motion;
        status.phase = "recording";
        status.has_recording_seconds = 1;
        status.recording_seconds = 10;
        status.has_input_level = 1;
        status.input_level = 0;
        view = project(status);
        pill_motion_init(&motion);
        pill_motion_step(&motion, &view, 1, 0);
        if (!view.recording || view.warning || !levels_equal(motion.levels, 0)) {
            fail("silence remains recording not an error");
        }
    }

    {
        const char *flags[] = {"muted", "clipping"};
        const char *titles[] = {"Microphone muted", "Input too loud"};
        for (int i = 0; i < 2; i++) {
            VoiceStatus status = blank();
            PillView view;
            status.phase = "recording";
            status.has_recording_seconds = 1;
            status.recording_seconds = 10;
            status.has_input_level = 1;
            status.input_level = 1;
            status.has_microphone = 1;
            if (i == 0) status.mic_muted = 1;
            else status.mic_clipping = 1;
            view = project(status);
            if (!view.expanded || strcmp(view.title, titles[i]) != 0 || strcmp(view.tone, "orange") != 0
                || (i == 0 && view.level != 0)) {
                fprintf(stderr, "subtest %s\n", flags[i]);
                fail("muted and clipping expand in place");
            }
        }
    }

    {
        VoiceStatus statuses[13];
        int compact[13];
        memset(statuses, 0, sizeof statuses);
        statuses[0].phase = "starting";
        statuses[1].phase = "stopping";
        statuses[2].phase = "transcribing";
        statuses[3].phase = "error";
        statuses[4].speaking = 1;
        statuses[4].audible = 1;
        statuses[5].draft = 1;
        statuses[6].retained = 1;
        statuses[7].queued = 1;
        statuses[8].rebind_needed = 1;
        statuses[9].models = "unavailable";
        statuses[10].connection_state = "reconnecting";
        statuses[11].agent_state = "blocked";
        statuses[12].osd = 1;
        statuses[12].osd_message = "Cancelled";
        for (int i = 0; i < 13; i++) {
            const char *phase = statuses[i].phase ? statuses[i].phase : "";
            compact[i] = strcmp(phase, "stopping") == 0 || strcmp(phase, "transcribing") == 0 || statuses[i].speaking;
            statuses[i].has_input_level = 1;
            statuses[i].input_level = 1;
        }
        for (int i = 0; i < 13; i++) {
            PillView view = project(statuses[i]);
            if (!view.visible || view.recording || view.level != 0 || view.expanded == compact[i]) {
                fprintf(stderr, "activity subtest %d title=%s expanded=%d\n", i, view.title, view.expanded);
                fail("work speech and attention never show input activity");
            }
        }
    }

    {
        VoiceStatus status = blank();
        PillView view;
        status.phase = "recording";
        status.has_recording_seconds = 1;
        status.recording_seconds = 4;
        status.speaking = 1;
        status.audible = 1;
        status.agent_state = "blocked";
        view = project(status);
        if (!view.recording || strcmp(view.tone, "red") != 0 || view.expanded) {
            fail("active capture wins over speech and attention");
        }
    }

    {
        VoiceStatus recording_status = blank();
        VoiceStatus next_status[2];
        PillView recording;
        recording_status.phase = "recording";
        recording_status.has_recording_seconds = 1;
        recording_status.recording_seconds = 10;
        recording_status.has_input_level = 1;
        recording_status.input_level = 1;
        recording = project(recording_status);
        memset(next_status, 0, sizeof next_status);
        next_status[0].phase = "transcribing";
        next_status[1].phase = "idle";
        for (int i = 0; i < 2; i++) {
            PillMotion motion;
            PillView next = project(next_status[i]);
            pill_motion_init(&motion);
            pill_motion_step(&motion, &recording, 1, 0);
            if (motion.levels[PILL_BARS - 1] != 1) fail("meter sample");
            pill_motion_step(&motion, &next, 1.1, 0);
            if (!levels_equal(motion.levels, 0)) fail("meter clears on phase change and hide");
        }
    }

    {
        VoiceStatus shown_status = blank();
        VoiceStatus hidden_status = blank();
        PillView shown;
        PillView hidden;
        PillMotion motion;
        int active = 0;
        shown_status.phase = "recording";
        shown_status.has_recording_seconds = 1;
        shown_status.recording_seconds = 10;
        shown = project(shown_status);
        hidden_status.phase = "idle";
        hidden = project(hidden_status);
        pill_motion_init(&motion);
        for (int frame = 0; frame < 120; frame++) {
            if (!pill_motion_step(&motion, &shown, frame / 60.0, 0)) fail("animation stays active while shown");
        }
        if (fabs(motion.width - 208.0) >= 5e-5) fail("animation converges");
        for (int frame = 120; frame < 240; frame++) {
            active = pill_motion_step(&motion, &hidden, frame / 60.0, 0);
        }
        if (active || !(motion.opacity < 0.01)) fail("animation stops when hidden");
    }

    {
        VoiceStatus shown_status = blank();
        PillView shown;
        PillMotion motion;
        shown_status.phase = "recording";
        shown_status.has_recording_seconds = 1;
        shown_status.recording_seconds = 10;
        shown = project(shown_status);
        pill_motion_init(&motion);
        pill_motion_step(&motion, &shown, 1, 1);
        if (motion.width != 208 || motion.height != 44 || motion.opacity != 1) {
            fail("reduced motion snaps to target");
        }
        {
            VoiceStatus idle = blank();
            PillView hidden;
            idle.phase = "idle";
            hidden = project(idle);
            if (pill_motion_step(&motion, &hidden, 2, 1)) fail("reduced motion hides immediately");
        }
    }

    {
        double numbers[] = {NAN, INFINITY, -1};
        for (int i = 0; i < 5; i++) {
            VoiceStatus status = blank();
            PillView view;
            status.phase = "recording";
            status.has_recording_seconds = 1;
            status.has_input_level = 1;
            if (i == 3) {
                status.recording_seconds_invalid = 1;
                status.input_level_invalid = 1;
            } else if (i == 4) {
                status.has_recording_seconds = 0;
                status.has_input_level = 0;
            } else {
                status.recording_seconds = numbers[i];
                status.input_level = numbers[i];
            }
            view = project(status);
            if (strcmp(view.timer, "00:00") != 0 || view.level < 0 || view.level > 1) {
                fail("nonfinite input and elapsed are safe");
            }
        }
    }

    {
        WarningFilter warnings;
        VoiceStatus status = blank();
        OsdView raw;
        OsdView out;
        OsdView cleared;
        warning_filter_init(&warnings);
        status.phase = "recording";
        status.has_recording_seconds = 1;
        status.recording_seconds = 8;
        status.has_microphone = 1;
        status.mic_clipping = 1;
        osd_view(&status, &raw);
        warning_filter_apply(&warnings, &raw, 0, &out);
        if (out.clipping) fail("clipping latch start");
        warning_filter_apply(&warnings, &raw, 1.5, &out);
        if (out.clipping) fail("clipping latch 1.5");
        warning_filter_apply(&warnings, &raw, 1.5, &out);
        if (strstr(out.detail, "clipping")) fail("clipping detail withheld");
        warning_filter_apply(&warnings, &raw, 1.7, &out);
        if (!out.clipping) fail("clipping confirmed at 1.7");
        cleared = raw;
        cleared.clipping = 0;
        warning_filter_apply(&warnings, &cleared, 1.8, &out);
        warning_filter_apply(&warnings, &raw, 2, &out);
        if (out.clipping) fail("clipping resets after a clear sample");
    }

    {
        WarningFilter warnings;
        VoiceStatus status = blank();
        OsdView raw;
        OsdView modified;
        OsdView out;
        OsdView idle;
        VoiceStatus empty = blank();
        warning_filter_init(&warnings);
        status.phase = "recording";
        status.has_recording_seconds = 1;
        status.recording_seconds = 0;
        status.has_microphone = 1;
        status.mic_clipping = 1;
        osd_view(&status, &raw);
        warning_filter_apply(&warnings, &raw, 0, &out);
        if (out.clipping) fail("startup grace start");
        modified = raw;
        modified.seconds = 1;
        warning_filter_apply(&warnings, &modified, 1.8, &out);
        if (out.clipping) fail("startup grace before 2s");
        modified = raw;
        modified.seconds = 2;
        warning_filter_apply(&warnings, &modified, 2.1, &out);
        if (!out.clipping) fail("startup grace confirms");
        warning_filter_apply(&warnings, &raw, 3, &out);
        if (out.clipping) fail("new recording seconds reset clipping");
        osd_view(&empty, &idle);
        warning_filter_apply(&warnings, &idle, 4, &out);
        modified = raw;
        modified.seconds = 10;
        warning_filter_apply(&warnings, &modified, 5, &out);
        if (out.clipping) fail("idle gap resets clipping");
    }

    {
        WarningFilter warnings;
        VoiceStatus status = blank();
        OsdView raw;
        OsdView filtered;
        PillView view;
        warning_filter_init(&warnings);
        status.phase = "recording";
        status.has_recording_seconds = 1;
        status.recording_seconds = 0;
        status.has_microphone = 1;
        status.mic_muted = 1;
        status.mic_clipping = 1;
        osd_view(&status, &raw);
        warning_filter_apply(&warnings, &raw, 0, &filtered);
        pill_view(&filtered, &view);
        if (strcmp(view.title, "Microphone muted") != 0 || strstr(view.detail, "clipping")) {
            fail("muting is immediate even during clipping grace");
        }
    }

    {
        PillMotion motion;
        pill_motion_init(&motion);
        for (int frame = 0; frame < 240; frame++) {
            VoiceStatus status = blank();
            PillView view;
            status.phase = "recording";
            status.has_recording_seconds = 1;
            status.recording_seconds = frame % 10;
            view = project(status);
            pill_motion_step(&motion, &view, frame / 60.0, 0);
            if (motion.width < 176 || motion.width > 352 || motion.height < 36 || motion.height > 76
                || motion.opacity < 0 || motion.opacity > 1) {
                fail("motion never overshoots geometry");
                break;
            }
        }
    }

    if (!paint_geometry()) fail("paint geometry");
    if (PILL_BARS != 17) fail("history bar count");
    return failures;
}
