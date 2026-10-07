#ifndef PI_VOICE_OSD_MAIN_H
#define PI_VOICE_OSD_MAIN_H

#include "pill.h"

#include <stddef.h>

/* Status-card window policy. Rendering never receives transcript or reply. */

typedef struct OsdStatusDoc OsdStatusDoc;

OsdStatusDoc *osd_status_parse(const char *json);
const VoiceStatus *osd_status_view(const OsdStatusDoc *doc);
void osd_status_free(OsdStatusDoc *doc);
int osd_status_contains(const OsdStatusDoc *doc, const char *needle);

int osd_notice_view(const char *json, double now_unix, OsdView *out);
void osd_unavailable(PillView *out);

/* have_status 0 is a failed read. was_recording stays set across failures. */
void osd_apply_sample(int have_status, const VoiceStatus *status, int *was_recording,
    WarningFilter *filter, double now_mono, const char *notice_json, double now_unix,
    PillView *out);
/* 1 when the sample is older than 2s and out replaced the card. */
int osd_apply_stale(const PillView *current, double sampled_mono, double now_mono, PillView *out);

int osd_monitors_parse(const char *json, VoiceMonitor **monitors, char ***names, size_t *count);
void osd_monitors_free(VoiceMonitor *monitors, char **names, size_t count);

void osd_theme_paths(const char *xdg_state, const char *home,
    char *shell, size_t shell_cap, char *waybar, size_t waybar_cap);
void osd_accessible_label(const PillView *view, char *out, size_t cap);
int osd_frame_continues(const PillView *view, const PillMotion *motion, int reduced, int keep);

#endif
