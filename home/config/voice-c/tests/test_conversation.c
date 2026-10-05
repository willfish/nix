#include "conversation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

typedef struct {
    int calls;
    char *argv0;
    char *last;
    char *log;
    int fail_at;
    const char *stdout_text;
} Proc;

static void append_log(Proc *state, const char *line) {
    size_t old = state->log ? strlen(state->log) : 0;
    size_t add = strlen(line) + 2;
    char *next = realloc(state->log, old + add);
    if (!next) return;
    state->log = next;
    snprintf(state->log + old, add, "%s\n", line);
}

static int proc(void *user, const char *const *argv, int timeout_sec, char **stdout_text, char **error) {
    Proc *state = user;
    size_t i;
    char joined[512];
    size_t used = 0;
    (void)timeout_sec;
    state->calls++;
    joined[0] = 0;
    for (i = 0; argv && argv[i]; i++) {
        int wrote = snprintf(joined + used, sizeof joined - used, "%s%s", i ? " " : "", argv[i]);
        if (wrote > 0) used += (size_t)wrote;
    }
    append_log(state, joined);
    free(state->last);
    state->last = strdup(joined);
    if (!state->argv0) state->argv0 = strdup(argv && argv[0] ? argv[0] : "");
    if (state->fail_at && state->calls == state->fail_at) {
        if (error) *error = strdup("systemctl failed");
        return -1;
    }
    if (stdout_text) *stdout_text = strdup(state->stdout_text ? state->stdout_text : "");
    return 0;
}

typedef struct {
    const char *action;
    int calls;
} Pick;

static int pick(void *user, const char *prompt, const VoiceMenuRow *rows, size_t count, char **action, char **error) {
    Pick *state = user;
    (void)prompt;
    (void)rows;
    (void)count;
    (void)error;
    state->calls++;
    if (!state->action) return 1;
    *action = strdup(state->action);
    return 0;
}

typedef struct {
    int active;
    int probes;
    int opens;
    const char *body;
} Ready;

static int ready_active(void *user, char **error) {
    (void)error;
    return ((Ready *)user)->active;
}

static int ready_probe(void *user, char **error) {
    Ready *ready = user;
    (void)error;
    ready->probes++;
    return ready->body && strstr(ready->body, "<title>PersonaPlex</title>") != NULL;
}

static int ready_open(void *user, const char *url, char **error) {
    (void)error;
    if (strcmp(url, VOICE_PERSONAPLEX_URL) != 0) return -1;
    ((Ready *)user)->opens++;
    return 0;
}

static double clock_values[] = {0, 0, 2};
static int clock_i;

static double clock_now(void *user) {
    (void)user;
    if (clock_i >= 3) return 2;
    return clock_values[clock_i++];
}

static void clock_sleep(void *user, double seconds) {
    (void)user;
    (void)seconds;
}

int test_conversation(void) {
    failures = 0;
    {
        Proc state = {0};
        VoiceConversation *conversation = voice_conversation_new(proc, &state);
        Pick chooser = {0};
        state.stdout_text = "active\n";
        if (!voice_conversation_active(conversation, NULL)) fail("active state");
        state.stdout_text = "inactive";
        if (voice_conversation_active(conversation, NULL)) fail("inactive state");
        state.stdout_text = "activating";
        if (!voice_conversation_active(conversation, NULL)) fail("activating state");
        {
            int calls = state.calls;
            if (voice_conversation_menu(conversation, pick, &chooser, NULL) || state.calls != calls) fail("cancel changes nothing");
        }
        chooser.action = "pi";
        if (voice_conversation_menu(conversation, pick, &chooser, NULL)
            || !state.log
            || !strstr(state.log, "systemctl --user stop personaplex-open.service personaplex.service\nsystemctl --user start pi-voice.service pi-voice-osd.service\n")) fail("switch back order");
        state.fail_at = state.calls + 1;
        if (voice_conversation_stop(conversation, NULL) == 0) fail("stop failure must not start pi");
        voice_conversation_free(conversation);
        conversation = voice_conversation_new(proc, &state);
        state.calls = 0;
        state.fail_at = 0;
        if (voice_conversation_start(conversation, NULL) || !state.last
            || strcmp(state.last, "systemctl --user restart --no-block personaplex-open.service")) fail("start opens waiter");
        voice_conversation_free(conversation);
        free(state.last);
        free(state.log);
        free(state.argv0);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        if (!voice_can_switch(status)) fail("idle can switch");
        status->draft = 1;
        if (voice_can_switch(status)) fail("draft blocks switch");
        status->draft = 0;
        status->pending = 1;
        if (voice_can_switch(status)) fail("pending blocks switch");
        status->pending = 0;
        status->retained = 1;
        if (voice_can_switch(status)) fail("retained blocks switch");
        status->retained = 0;
        status->phase = strdup("recording");
        if (voice_can_switch(status)) fail("recording blocks switch");
        voice_menu_status_free(status);
    }
    {
        Ready ready = {1, 0, 0, "<title>PersonaPlex</title>"};
        VoiceReadyHooks hooks = {ready_active, ready_probe, ready_open, clock_sleep, clock_now};
        clock_i = 0;
        clock_values[0] = 0;
        clock_values[1] = 0;
        clock_values[2] = 0;
        if (voice_open_when_ready(&hooks, &ready, 240, NULL) || ready.opens != 1) fail("open after ready");
        ready.body = "<title>Other service</title>";
        ready.opens = 0;
        clock_i = 0;
        clock_values[2] = 2;
        {
            char *error = NULL;
            if (voice_open_when_ready(&hooks, &ready, 1, &error) == 0 || !error
                || !strstr(error, "did not become ready") || ready.opens) fail("unrelated http");
            free(error);
        }
        ready.active = 0;
        clock_i = 0;
        clock_values[1] = 0;
        {
            char *error = NULL;
            if (voice_open_when_ready(&hooks, &ready, 240, &error) == 0 || !error
                || !strstr(error, "stopped or failed")) fail("backend failure");
            free(error);
        }
    }
    {
        char dir[] = "/tmp/voice-ui-conv-XXXXXX";
        char path[256];
        char saved_path[4096];
        char log_path[256];
        char logged[512] = {0};
        FILE *script;
        FILE *log;
        const char *old_path = getenv("PATH");
        VoiceConversation *conversation;
        char *error = NULL;
        if (!mkdtemp(dir)) fail("null runner is real");
        snprintf(path, sizeof path, "%s/systemctl", dir);
        snprintf(log_path, sizeof log_path, "%s/log", dir);
        script = fopen(path, "w");
        if (script) {
            fprintf(script, "#!/bin/sh\nprintf '%%s\\n' \"$*\" >> '%s'\nprintf 'inactive\\n'\n", log_path);
            fclose(script);
            chmod(path, 0700);
        }
        snprintf(saved_path, sizeof saved_path, "%s", old_path ? old_path : "");
        setenv("PATH", dir, 1);
        conversation = voice_conversation_new(NULL, NULL);
        if (!conversation || voice_conversation_active(conversation, &error) != 0
            || (error && strstr(error, "runner is unavailable"))) fail("null runner is real");
        free(error);
        log = fopen(log_path, "r");
        if (!log || !fgets(logged, sizeof logged, log)
            || strcmp(logged, "--user show personaplex.service --property=ActiveState --value\n")) fail("null runner is real");
        if (log) fclose(log);
        voice_conversation_free(conversation);
        if (old_path) setenv("PATH", saved_path, 1);
        else unsetenv("PATH");
        remove(path);
        remove(log_path);
        rmdir(dir);
    }
    return failures;
}
