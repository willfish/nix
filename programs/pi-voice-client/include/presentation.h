#ifndef PI_VOICE_PRESENTATION_H
#define PI_VOICE_PRESENTATION_H

#include <stddef.h>

/* Public menu card. Transcript and reply text are never stored. */

#define VOICE_LABEL_UNLIMITED ((size_t)-1)

typedef struct {
    char *token;
    char *id;
    char *label;
    char *full_label;
    char *thinking;
    char *connection_state; /* NULL means the controller omitted it (ready) */
    int selected;
    int team_child;
} VoiceSessionRow;

typedef struct {
    char *key;
    char *label;
} VoiceNamed;

typedef struct {
    int present; /* non-empty microphone object */
    char *name;
    int muted;
    int clipping;
    int missing;
    char *error;
} VoiceMicrophone;

typedef struct VoiceMenuStatus {
    char *phase; /* NULL means omitted, which presentation treats as idle */
    int preparing;
    char *pane;
    int draft; /* truthy only; dictated text is not retained */
    int pending;
    int retry;
    int speaking;
    int responding;
    char *agent_state;
    char *connection_state;
    int retained;
    char *retained_source;
    char *harness; /* raw controller value, not the display name */
    int has_recording_seconds;
    double recording_seconds;
    int has_input_level;
    double input_level;
    char *error;
    char *recording_label;
    char *session_label;
    int selection_explicit_missing; /* omitted key defaults to explicit */
    int selection_explicit;
    int show_team;
    int auto_missing;
    int auto_value;
    char *selected_voice;
    char *selected_stt;
    int speech_available_missing; /* omitted defaults to available */
    int speech_available;
    int can_speak_missing; /* omitted defaults to can speak */
    int can_speak;
    char *models;
    char *model_error;
    int rebind_needed;
    int has_reply; /* presence only */
    char *speech_backend;
    int conversation_available;
    VoiceSessionRow *sessions;
    size_t session_count;
    VoiceNamed *voices;
    size_t voice_count;
    VoiceNamed *speech_backends;
    size_t speech_count;
    VoiceNamed *stt_backends;
    size_t stt_count;
    VoiceMicrophone microphone;
} VoiceMenuStatus;

typedef struct {
    char *action;
    char *label;
    int enabled;
} VoiceAction;

typedef struct {
    char *action;
    char *token;
    char *id;
} VoiceIdentity;

typedef struct {
    char *action;
    char *label;
} VoiceFullLabel;

typedef struct VoicePresentation {
    char *label;
    char *colour;
    char *glyph;
    char **context;
    size_t context_count;
    int auto_on;
    char *selected_voice;
    char *selected_stt;
    char *selected_session;
    VoiceIdentity *identities;
    size_t identity_count;
    VoiceFullLabel *full_labels;
    size_t full_label_count;
    int show_team;
    VoiceAction *actions;
    size_t action_count;
} VoicePresentation;

/* Owned strings. Missing tri-state flags match controller .get defaults. */
VoiceMenuStatus *voice_menu_status_new(void);
void voice_menu_status_free(VoiceMenuStatus *status);
int voice_menu_add_session(VoiceMenuStatus *status, const char *token, const char *id,
    const char *label, const char *full_label, const char *thinking,
    const char *connection_state, int selected, int team_child);
int voice_menu_add_named(VoiceNamed **items, size_t *count, const char *key, const char *label);
int voice_menu_status_from_json(const char *json, VoiceMenuStatus **out, char **error);
int voice_menu_status_contains(const VoiceMenuStatus *status, const char *needle);

/* limit is code points. VOICE_LABEL_UNLIMITED keeps the whole printable label.
   Returns 0, or -1 if out is too small for the bounded result. */
int voice_public_label(const char *value, size_t limit, char *out, size_t cap);

int voice_presentation(const VoiceMenuStatus *status, VoicePresentation **out, char **error);
void voice_presentation_free(VoicePresentation *view);
const VoiceAction *voice_presentation_action(const VoicePresentation *view, const char *action);
int voice_presentation_contains(const VoicePresentation *view, const char *needle);
int voice_menu_busy(const VoiceMenuStatus *status);

/* Selected (token, id) rows, including hidden team children. 0 when equal. */
int voice_selected_same(const VoiceMenuStatus *left, const VoiceMenuStatus *right);

#endif
