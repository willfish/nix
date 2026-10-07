#include "pill.h"

#include <math.h>
#include <string.h>

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static double finite_number(double value) {
    return isfinite(value) ? value : 0.0;
}

static int phase_is(const char *phase, const char *want) {
    return phase && strcmp(phase, want) == 0;
}

void pill_view(const OsdView *view, PillView *out) {
    OsdView empty;
    int recording;
    int muted;
    int clipping;
    int warning;
    int compact;
    const char *title;
    double seconds;
    if (!out) return;
    memset(out, 0, sizeof *out);
    memset(&empty, 0, sizeof empty);
    if (!view) view = &empty;
    recording = view->recording;
    title = view->title;
    muted = recording && view->muted;
    clipping = recording && view->clipping;
    warning = muted || clipping;
    if (muted) title = "Microphone muted";
    else if (clipping) title = "Input too loud";
    compact = phase_is(view->phase, "stopping") || phase_is(view->phase, "transcribing")
        || strcmp(title, "Speaking") == 0 || strcmp(title, "About to speak") == 0;
    seconds = finite_number(view->seconds);
    out->visible = view->visible;
    out->recording = recording;
    if (muted || !recording) out->level = 0;
    else {
        double level = finite_number(view->level);
        if (level < 0) level = 0;
        if (level > 1) level = 1;
        out->level = level;
    }
    copy_text(out->title, sizeof out->title, title[0] ? title : "Voice");
    copy_text(out->detail, sizeof out->detail, view->detail);
    copy_text(out->tone, sizeof out->tone, warning ? "orange" : (view->tone[0] ? view->tone : "muted"));
    copy_text(out->timer, sizeof out->timer, view->timer[0] ? view->timer : "00:00");
    out->warning = warning;
    out->muted = muted;
    out->working = phase_is(view->phase, "starting") || phase_is(view->phase, "stopping")
        || phase_is(view->phase, "transcribing");
    out->expanded = view->visible && (
        warning
        || (recording && seconds < 2.0)
        || (!recording && !compact));
    out->width = out->expanded ? 352.0 : 208.0;
    out->height = out->expanded ? 76.0 : 44.0;
    copy_text(out->phase, sizeof out->phase, view->phase);
}

void warning_filter_init(WarningFilter *filter) {
    if (!filter) return;
    memset(filter, 0, sizeof *filter);
}

static void remove_suffix(char *text, const char *suffix) {
    size_t n;
    size_t m;
    if (!text || !suffix) return;
    n = strlen(text);
    m = strlen(suffix);
    if (n >= m && memcmp(text + n - m, suffix, m) == 0) text[n - m] = 0;
}

void warning_filter_apply(WarningFilter *filter, const OsdView *in, double now, OsdView *out) {
    double seconds;
    WarningFilter local;
    if (!filter) {
        warning_filter_init(&local);
        filter = &local;
    }
    if (!out) return;
    if (!in) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = *in;
    seconds = finite_number(out->seconds);
    if (!out->recording || (filter->has_last_seconds && seconds < filter->last_seconds)) {
        filter->has_clipping_since = 0;
    }
    if (out->recording) {
        filter->has_last_seconds = 1;
        filter->last_seconds = seconds;
    } else {
        filter->has_last_seconds = 0;
    }
    if (!out->recording || !out->clipping) {
        filter->has_clipping_since = 0;
        return;
    }
    if (!filter->has_clipping_since) {
        filter->has_clipping_since = 1;
        filter->clipping_since = now;
    }
    /* Capture latches each peak for 1.5s. Outwait that latch, and the first 2s. */
    if (!(seconds >= 2.0 && now - filter->clipping_since >= 1.6)) {
        out->clipping = 0;
        remove_suffix(out->detail, out->muted ? ", clipping" : " \xc2\xb7 clipping");
    }
}

void pill_motion_init(PillMotion *motion) {
    if (!motion) return;
    memset(motion, 0, sizeof *motion);
    motion->width = 176.0;
    motion->height = 36.0;
}

int pill_motion_step(PillMotion *motion, const PillView *view, double now, int reduced) {
    double dt;
    double target_w;
    double target_h;
    double target_o;
    double ease;
    if (!motion || !view) return 0;
    dt = motion->has_last ? fmin(0.1, fmax(0.0, now - motion->last)) : (1.0 / 60.0);
    motion->last = now;
    motion->has_last = 1;
    target_w = view->visible ? view->width : 176.0;
    target_h = view->visible ? view->height : 36.0;
    target_o = view->visible ? 1.0 : 0.0;
    ease = reduced ? 1.0 : 1.0 - exp(-dt / 0.085);
    motion->width += (target_w - motion->width) * ease;
    motion->height += (target_h - motion->height) * ease;
    motion->opacity += (target_o - motion->opacity) * ease;
    if (!view->visible || !view->recording || view->muted) {
        memset(motion->levels, 0, sizeof motion->levels);
        motion->has_sample = 0;
    } else if (!motion->has_sample || now - motion->sample_at >= 0.065) {
        double level = view->level;
        double scaled;
        if (!isfinite(level) || level < 0) level = 0;
        scaled = (20.0 * log10(fmax(1e-6, level)) + 50.0) / 40.0;
        if (scaled < 0) scaled = 0;
        if (scaled > 1) scaled = 1;
        memmove(motion->levels, motion->levels + 1, sizeof(double) * (PILL_BARS - 1));
        motion->levels[PILL_BARS - 1] = scaled;
        motion->sample_at = now;
        motion->has_sample = 1;
    }
    return view->visible || motion->opacity > 0.01;
}
