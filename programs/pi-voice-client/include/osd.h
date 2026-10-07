#ifndef PI_VOICE_OSD_H
#define PI_VOICE_OSD_H

#include <stddef.h>

/* Public dictation card. Transcript and reply text are not represented. */

typedef struct {
    const char *name;
    int focused; /* exactly 1 counts as focused */
} VoiceMonitor;

typedef struct {
    int selected;
    const char *full_label;
    const char *label;
} VoiceSession;

typedef struct {
    const char *phase;
    int osd;
    const char *pane;
    const char *recording_label;
    const char *session_label;
    const char *osd_message; /* NULL unless the value is a string */
    const char *osd_tone;
    const char *error;
    const char *agent_state;
    const char *connection_state;
    const char *models;
    const char *model_error;
    const char *retained_source;
    int queued;
    int pending;
    int draft;
    int draft_edited;
    int retained;
    int speaking;
    int audible;
    int rebind_needed;
    int has_recording_seconds;
    int recording_seconds_invalid;
    double recording_seconds;
    int has_input_level;
    int input_level_invalid;
    double input_level;
    int has_microphone;
    int mic_muted;
    int mic_clipping;
    const VoiceSession *sessions;
    size_t session_count;
} VoiceStatus;

typedef struct {
    char background[16];
    char text[16];
    char border[16];
    char accent[16];
    char muted[16];
    char red[16];
    char yellow[16];
    char green[16];
    char orange[16];
    char teal[16];
} VoiceColours;

typedef struct {
    int visible;
    char title[160];
    char detail[240];
    char tone[16];
    double level;
    char meter[40];
    int recording;
    int transcribing;
    char phase[32];
    double seconds;
    char timer[32];
    int muted;
    int clipping;
} OsdView;

typedef struct {
    int ok_is_true;
    const char *phase;
    const char *reply; /* accepted and dropped; never copied */
} VoiceStatusPayload;

const char *focused_connector(const VoiceMonitor *monitors, size_t count);
void popup_colours(const char *text, VoiceColours *out);
void resolved_colours(const char *shell_text, const char *waybar_text, VoiceColours *out);
const char *level_to_block(double level);
void meter_blocks(const double *levels, size_t count, char *out, size_t out_cap);
void osd_view(const VoiceStatus *status, OsdView *out);
/* Writes public fields and returns 1. Returns 0 when ok is not exactly true. */
int status_from_response(const VoiceStatusPayload *payload, VoiceStatus *out);

#endif
