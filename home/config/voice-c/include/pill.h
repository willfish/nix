#ifndef PI_VOICE_PILL_H
#define PI_VOICE_PILL_H

#include "osd.h"

#include <cairo.h>

#define PILL_BARS 17
#define PILL_SURFACE_WIDTH 420
#define PILL_SURFACE_HEIGHT 104

typedef struct {
    int visible;
    int recording;
    double level;
    char title[160];
    char detail[240];
    char tone[16];
    char timer[32];
    int warning;
    int muted;
    int working;
    int expanded;
    double width;
    double height;
    char phase[32];
} PillView;

typedef struct {
    int has_clipping_since;
    double clipping_since;
    int has_last_seconds;
    double last_seconds;
} WarningFilter;

typedef struct {
    double width;
    double height;
    double opacity;
    double levels[PILL_BARS];
    int has_last;
    double last;
    int has_sample;
    double sample_at;
} PillMotion;

void pill_view(const OsdView *view, PillView *out);
void warning_filter_init(WarningFilter *filter);
void warning_filter_apply(WarningFilter *filter, const OsdView *in, double now, OsdView *out);
void pill_motion_init(PillMotion *motion);
/* Returns 1 while the surface should stay up. Reduced motion snaps geometry. */
int pill_motion_step(PillMotion *motion, const PillView *view, double now, int reduced);
void pill_paint(cairo_t *cr, const PillView *view, const PillMotion *motion,
    const VoiceColours *colours, double now, int reduced);

#endif
