#ifndef PI_VOICE_PROTOCOL_H
#define PI_VOICE_PROTOCOL_H

/* Voice control protocol. Action names, status types and selection.json keys
 * match the Python controller. Responses are one JSON object per line.
 * Dictation text is never placed in OSD, notices, or logs. */

#include <stddef.h>

#define VOICE_REQUEST_MAX (1024 * 1024)
#define VOICE_SOCKET_NAME "control.sock"
#define VOICE_SELECTION_NAME "selection.json"
#define VOICE_NOTICE_MAX 160
#define VOICE_RETRY_SECONDS 120.0
#define VOICE_RECORD_MAX_SECONDS 185.0
#define VOICE_MONITOR_SECONDS 2.0
#define VOICE_ACTIVITY_INTERVAL 1.0
#define VOICE_PROVISIONAL_POLLS 40
#define VOICE_PROVISIONAL_SLEEP 0.25
#define VOICE_ACCEPT_BACKLOG 16
#define VOICE_HANDLER_THREADS 8

/* Selection object keys. */
#define VOICE_KEY_TOKEN "token"
#define VOICE_KEY_SHOW_TEAM "show_team"
#define VOICE_KEY_SELECTION_INITIALIZED "selection_initialized"
#define VOICE_KEY_SELECTION_EXPLICIT "selection_explicit"
#define VOICE_KEY_RECONNECT_PANE "reconnect_pane"
#define VOICE_KEY_RECONNECT_EXPLICIT "reconnect_explicit"
#define VOICE_KEY_FENCES "attachment_fences"
#define VOICE_KEY_HIGHWATER "highwater"
#define VOICE_KEY_RETIRED "retired"
#define VOICE_KEY_TARGET "target"
#define VOICE_KEY_THREAD "thread"
#define VOICE_KEY_TURNS "turns"
#define VOICE_KEY_SESSIONS "sessions"
#define VOICE_KEY_CANDIDATE "candidate"
#define VOICE_KEY_EXCLUDED "excluded"

/* Owned envelope: {"ok":true,...} or {"ok":false,"error":"..."}. */
char *voice_envelope_ok(const char *object_json);
char *voice_envelope_error(const char *message);
char *voice_envelope_fields(const char *key, const char *object_json, int bool_value, int has_bool);

int voice_tone_ok(const char *tone);
void voice_public_text(const char *input, char *out, size_t cap);

#endif
