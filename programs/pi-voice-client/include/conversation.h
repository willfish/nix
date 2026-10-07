#ifndef PI_VOICE_CONVERSATION_H
#define PI_VOICE_CONVERSATION_H

#include "menu.h"

/* PersonaPlex is mutually exclusive with Pi dictation. No Pi socket calls here. */

#define VOICE_PERSONAPLEX_URL "http://127.0.0.1:8998"

typedef int (*voice_proc_fn)(void *user, const char *const *argv, int timeout_sec,
    char **stdout_text, char **error);

typedef struct VoiceConversation VoiceConversation;

/* NULL run uses ipc_process_run. It is not a stub, including in the library build. */
VoiceConversation *voice_conversation_new(voice_proc_fn run, void *user);
void voice_conversation_free(VoiceConversation *conversation);
int voice_conversation_active(VoiceConversation *conversation, char **error);
int voice_conversation_start(VoiceConversation *conversation, char **error);
int voice_conversation_open(VoiceConversation *conversation, char **error);
int voice_conversation_stop(VoiceConversation *conversation, char **error);
int voice_conversation_menu(VoiceConversation *conversation, voice_pick_fn pick,
    void *pick_user, char **error);
int voice_can_switch(const VoiceMenuStatus *status);

typedef struct {
    int (*active)(void *user, char **error);
    int (*probe)(void *user, char **error); /* 1 ready, 0 retry, -1 failed */
    int (*open_url)(void *user, const char *url, char **error);
    void (*sleep_sec)(void *user, double seconds);
    double (*monotonic)(void *user);
} VoiceReadyHooks;

int voice_open_when_ready(const VoiceReadyHooks *hooks, void *user, double timeout_sec, char **error);
/* 1 ready, 0 not yet, -1 failed. Network failures are not yet. */
int voice_personaplex_probe(char **error);

#endif
