#ifndef PI_VOICE_MENU_H
#define PI_VOICE_MENU_H

#include "presentation.h"

#include <stddef.h>

/* Keyboard menu over the public presentation. Dispatch actions stay stable. */

typedef struct {
    char *action;
    char *label;
} VoiceMenuRow;

/* Ownership of *out:
 * On 0, publish a fresh status and transfer it. voice_menu_run frees every status it
 * receives, including the one freed before the next status request when entering a
 * submenu (menu:*). Do not retain an alias, do not reuse the object, and do not free it.
 * On non-zero, leave *out NULL. A failure must not publish a partial or borrowed object.
 * Non-status actions may publish a fresh status or NULL; the runner frees a non-NULL result.
 */
typedef int (*voice_request_fn)(void *user, const char *action, VoiceMenuStatus **out, char **error);
/* 0 selected, 1 cancelled, -1 failed. action is owned by the caller on 0. */
typedef int (*voice_pick_fn)(void *user, const char *prompt, const VoiceMenuRow *rows,
    size_t count, char **action, char **error);

typedef struct {
    int (*active)(void *user, char **error); /* 1 active, 0 idle, -1 failed */
    int (*menu)(void *user, voice_pick_fn pick, void *pick_user, char **error);
    int (*start)(void *user, char **error);
} VoiceMenuConversation;

int voice_menu_rows(const VoiceMenuStatus *status, const char *section,
    VoiceMenuRow **rows, size_t *count, char **error);
void voice_menu_rows_free(VoiceMenuRow *rows, size_t count);
int voice_menu_prompt(const VoiceMenuStatus *status, const char *section, char **out, char **error);
const char *voice_menu_empty_reason(const VoiceMenuStatus *status, const char *section);
int voice_session_description(const VoiceSessionRow *row, char **out, char **error);

int voice_menu_run(const char *section, voice_request_fn request, void *request_user,
    voice_pick_fn pick, void *pick_user, const VoiceMenuConversation *conversation,
    void *conversation_user, char **error);

/* Fuzzel index mapping. argv_out and input_out are optional owned results. */
int voice_menu_fuzzel_plan(const char *prompt, const VoiceMenuRow *rows, size_t count,
    const char *config, char ***argv_out, char **input_out, char **error);
int voice_menu_interpret_fuzzel(int exit_code, const char *stdout_text, const char *stderr_text,
    size_t row_count, char **action, const VoiceMenuRow *rows, char **error);

typedef int (*voice_spawn_fn)(void *user, char *const *argv, const char *input,
    char **stdout_text, char **stderr_text, int *exit_code, char **error);
int voice_menu_pick_fuzzel(const char *prompt, const VoiceMenuRow *rows, size_t count,
    const char *config, voice_spawn_fn spawn, void *user, char **action, char **error);

#endif
