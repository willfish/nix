#include "menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

static VoiceMenuStatus *status_new(void) {
    VoiceMenuStatus *status = voice_menu_status_new();
    status->pane = strdup("pane-a");
    status->harness = strdup("pi");
    status->phase = strdup("idle");
    status->auto_missing = 0;
    status->auto_value = 1;
    voice_menu_add_named(&status->voices, &status->voice_count, "samantha", "Samantha");
    voice_menu_add_named(&status->voices, &status->voice_count, "data", "Data");
    voice_menu_add_session(status, "a", "thread-a", "same label", NULL, NULL, NULL, 1, 0);
    voice_menu_add_session(status, "b", "thread-b", "same label", NULL, NULL, NULL, 0, 0);
    return status;
}

static VoiceMenuStatus *clone_basic(const VoiceMenuStatus *src) {
    VoiceMenuStatus *copy = status_new();
    (void)src;
    return copy;
}

typedef struct {
    VoiceMenuStatus *first;
    VoiceMenuStatus *second;
    int calls;
    char *last;
} Script;

static int script_request(void *user, const char *action, VoiceMenuStatus **out, char **error) {
    Script *script = user;
    (void)error;
    script->calls++;
    free(script->last);
    script->last = strdup(action);
    if (strcmp(action, "status") == 0) {
        /* Transfer, matching a freshly parsed controller response. */
        *out = script->calls == 1 ? script->first : script->second;
        if (script->calls == 1) script->first = NULL;
        else script->second = NULL;
        return 0;
    }
    *out = voice_menu_status_new();
    return 0;
}

typedef struct {
    const char *action;
    char *prompt;
    int calls;
} Picker;

static int script_pick(void *user, const char *prompt, const VoiceMenuRow *rows, size_t count, char **action, char **error) {
    Picker *picker = user;
    (void)rows;
    (void)count;
    (void)error;
    picker->calls++;
    free(picker->prompt);
    picker->prompt = strdup(prompt);
    if (!picker->action) return 1;
    *action = strdup(picker->action);
    picker->action = NULL;
    return 0;
}

static int personaplex_active(void *user, char **error) {
    (void)user;
    (void)error;
    return 1;
}

static int start_calls;

static int personaplex_start(void *user, char **error) {
    (void)user;
    (void)error;
    start_calls++;
    return 0;
}

static int has_row(const VoiceMenuRow *rows, size_t count, const char *action) {
    size_t i;
    for (i = 0; i < count; i++) if (strcmp(rows[i].action, action) == 0) return 1;
    return 0;
}

static const char *row_text(const VoiceMenuRow *rows, size_t count, const char *action) {
    size_t i;
    for (i = 0; i < count; i++) if (strcmp(rows[i].action, action) == 0) return rows[i].label;
    return NULL;
}

int test_menu(void) {
    failures = 0;
    {
        VoiceMenuStatus *status = status_new();
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        status->sessions[1].team_child = 1;
        if (voice_menu_rows(status, "sessions", &rows, &count, NULL) || count != 1
            || strcmp(rows[0].action, "select:a")) fail("hidden team row");
        voice_menu_rows_free(rows, count);
        rows = NULL;
        if (voice_menu_rows(status, "menu", &rows, &count, NULL)
            || !has_row(rows, count, "team-toggle")
            || !row_text(rows, count, "team-toggle")
            || strcmp(row_text(rows, count, "team-toggle"), "Show team members: off")) fail("team toggle off");
        voice_menu_rows_free(rows, count);
        {
            Script script = { status, clone_basic(status), 0, NULL };
            Picker picker = { "team-toggle", NULL, 0 };
            script.second->show_team = status->show_team;
            if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, NULL)
                || strcmp(script.last, "team-toggle")) fail("team toggle dispatch");
            free(script.last);
            script.last = NULL;
            free(picker.prompt);
            voice_menu_status_free(script.second);
        }
    }
    {
        VoiceMenuStatus *old = status_new();
        VoiceMenuStatus *fresh = status_new();
        Script script;
        Picker picker = { "select:a", NULL, 0 };
        char *error = NULL;
        old->retained = fresh->retained = 1;
        old->selection_explicit_missing = fresh->selection_explicit_missing = 0;
        free(fresh->sessions[0].id);
        fresh->sessions[0].id = strdup("replacement");
        script.first = old;
        script.second = fresh;
        script.calls = 0;
        script.last = NULL;
        if (voice_menu_run("sessions", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "session changed")) fail("changed identity confirmation");
        free(error);
        free(script.last);
        script.last = NULL;
        free(picker.prompt);
        voice_menu_status_free(script.second);
    }
    {
        VoiceMenuStatus *old = status_new();
        VoiceMenuStatus *fresh = status_new();
        Script script = { old, fresh, 0, NULL };
        Picker picker = { "select:b", NULL, 0 };
        char *error = NULL;
        free(fresh->sessions[1].id);
        fresh->sessions[1].id = strdup("replacement");
        if (voice_menu_run("sessions", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "session changed")) fail("replaced token identity");
        free(error);
        free(script.last);
        script.last = NULL;
        free(picker.prompt);
    }
    {
        VoiceMenuStatus *fresh = status_new();
        Script script = { status_new(), fresh, 0, NULL };
        Picker picker = { "select:b", NULL, 0 };
        free(fresh->sessions[1].label);
        fresh->sessions[1].label = strdup("renamed");
        if (voice_menu_run("sessions", script_request, &script, script_pick, &picker, NULL, NULL, NULL)
            || strcmp(script.last, "select:b")) fail("label refresh keeps identity");
        free(script.last);
        script.last = NULL;
        free(picker.prompt);
    }
    {
        VoiceMenuStatus *status = status_new();
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        char *prompt = NULL;
        VoiceSessionRow row = {0};
        row.full_label = "pi · work · window · review · high · model · selected";
        row.thinking = "high";
        row.selected = 1;
        status->session_label = strdup("01a0e6d4-6e91-7403-b822-e7357e8775b0");
        free(status->sessions[0].full_label);
        status->sessions[0].full_label = strdup("pi · dot · 1 · medium · grok-4.7 · selected");
        status->sessions[0].thinking = strdup("medium");
        if (voice_menu_prompt(status, "menu", &prompt, NULL) || strcmp(prompt, "Voice | dot · 1 · grok-4.7"))
            fail("prompt replaces uuid");
        free(prompt);
        if (voice_menu_rows(status, "sessions", &rows, &count, NULL)
            || !row_text(rows, count, "select:a")
            || strcmp(row_text(rows, count, "select:a"), "* dot · 1 · grok-4.7")) fail("session row description");
        voice_menu_rows_free(rows, count);
        if (voice_session_description(&row, &prompt, NULL) || strcmp(prompt, "work · window · review · model"))
            fail("session description drops flags");
        free(prompt);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *old = status_new();
        VoiceMenuStatus *fresh = status_new();
        Script script = { old, fresh, 0, NULL };
        Picker picker = { "recover-stage", NULL, 0 };
        char *error = NULL;
        old->retained = fresh->retained = 1;
        old->selection_explicit_missing = fresh->selection_explicit_missing = 0;
        old->selection_explicit = fresh->selection_explicit = 1;
        free(fresh->sessions[0].id);
        fresh->sessions[0].id = strdup("replacement");
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "session changed")) fail("stage rechecks identity");
        free(error);
        free(script.last);
        script.last = NULL;
        free(picker.prompt);
    }
    {
        VoiceMenuStatus *state = status_new();
        Script script;
        Picker picker = { "stop", NULL, 0 };
        state->connection_state = strdup("connecting");
        script.first = state;
        script.second = voice_menu_status_new();
        script.calls = 0;
        script.last = NULL;
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, NULL)
            || script.calls != 2 || strcmp(script.last, "stop")) fail("stop skips fresh status");
        free(script.last);
        script.last = NULL;
        free(picker.prompt);
        voice_menu_status_free(script.second);
    }
    {
        VoiceMenuStatus *state = status_new();
        Script script = { state, NULL, 0, NULL };
        Picker picker = { NULL, NULL, 0 };
        free(state->phase);
        state->phase = strdup("recording");
        state->recording_label = strdup("pinned destination");
        state->session_label = strdup("new destination");
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, NULL)
            || !picker.prompt || !strstr(picker.prompt, "pinned destination")
            || strstr(picker.prompt, "new destination") || script.calls != 1) fail("pinned prompt and cancel");
        free(picker.prompt);
        free(script.last);
        script.last = NULL;
    }
    {
        VoiceMenuStatus *busy = status_new();
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        char *error = NULL;
        Script script;
        Picker picker = { NULL, NULL, 0 };
        free(busy->phase);
        busy->phase = strdup("recording");
        if (voice_menu_rows(busy, "sessions", &rows, &count, NULL) || count) fail("busy sessions hidden");
        voice_menu_rows_free(rows, count);
        voice_menu_add_named(&busy->stt_backends, &busy->stt_count, "whisper", "Whisper");
        voice_menu_add_named(&busy->stt_backends, &busy->stt_count, "deepgram", "Deepgram");
        rows = NULL;
        if (voice_menu_rows(busy, "dictation", &rows, &count, NULL) || count) fail("busy dictation hidden");
        voice_menu_rows_free(rows, count);
        rows = NULL;
        if (voice_menu_rows(busy, "menu", &rows, &count, NULL) || !count || strcmp(rows[0].action, "stop")
            || strcmp(rows[0].label, "Cancel recording") || has_row(rows, count, "menu:sessions")
            || has_row(rows, count, "menu:dictation") || !has_row(rows, count, "menu:voices"))
            fail("phase controls first");
        voice_menu_rows_free(rows, count);
        script.first = busy;
        script.second = NULL;
        script.calls = 0;
        script.last = NULL;
        if (voice_menu_run("dictation", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "locked")) fail("locked dictation notice");
        free(error);
        free(script.last);
        free(picker.prompt);
        script.last = NULL;
        picker.prompt = NULL;
        error = NULL;
        script.first = status_new();
        script.second = NULL;
        script.calls = 0;
        if (voice_menu_run("dictation", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "No dictation backends")) fail("missing dictation notice");
        free(error);
        free(script.last);
        free(picker.prompt);
        script.last = NULL;
    }
    {
        VoiceMenuStatus *state = status_new();
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        state->selected_stt = strdup("whisper");
        voice_menu_add_named(&state->stt_backends, &state->stt_count, "whisper", "Whisper (local GPU)");
        voice_menu_add_named(&state->stt_backends, &state->stt_count, "deepgram", "Deepgram (cloud)");
        if (voice_menu_rows(state, "dictation", &rows, &count, NULL) || count != 2
            || strcmp(rows[0].label, "* Whisper (local GPU)") || strcmp(rows[1].label, "Deepgram (cloud)"))
            fail("dictation mark");
        voice_menu_rows_free(rows, count);
        rows = NULL;
        free(state->speech_backend);
        state->speech_backend = strdup("local");
        voice_menu_add_named(&state->speech_backends, &state->speech_count, "local", "Local");
        voice_menu_add_named(&state->speech_backends, &state->speech_count, "deepgram", "Deepgram");
        if (voice_menu_rows(state, "speech", &rows, &count, NULL) || !row_text(rows, count, "speech:local")
            || strncmp(row_text(rows, count, "speech:local"), "* ", 2)) fail("speech mark");
        voice_menu_rows_free(rows, count);
        voice_menu_status_free(state);
    }
    {
        VoiceMenuStatus *old = status_new();
        VoiceMenuStatus *fresh = status_new();
        Script script = { old, fresh, 0, NULL };
        Picker picker = { "read", NULL, 0 };
        char *error = NULL;
        old->has_reply = fresh->has_reply = 1;
        free(fresh->sessions[0].id);
        fresh->sessions[0].id = strdup("different-thread");
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "session changed")) fail("stale read target");
        free(error);
        free(script.last);
        free(picker.prompt);
        script.last = NULL;
        picker.prompt = NULL;
        error = NULL;
        old = status_new();
        fresh = status_new();
        old->has_reply = 1;
        script.first = old;
        script.second = fresh;
        script.calls = 0;
        picker.action = "read";
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "no longer available")) fail("stale unavailable action");
        free(error);
        free(script.last);
        free(picker.prompt);
        script.last = NULL;
    }
    {
        VoiceMenuStatus *fresh = status_new();
        Script script = { status_new(), fresh, 0, NULL };
        Picker picker = { "auto-toggle", NULL, 0 };
        char *error = NULL;
        fresh->auto_value = 0;
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, &error) == 0
            || !error || !strstr(error, "setting changed")) fail("stale auto toggle");
        free(error);
        free(script.last);
        free(picker.prompt);
        script.last = NULL;
    }
    {
        VoiceMenuStatus *held = status_new();
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        char *prompt = NULL;
        char **argv = NULL;
        char *input = NULL;
        char *action = NULL;
        char *error = NULL;
        VoiceMenuRow sample[2];
        held->retained = 1;
        held->selection_explicit_missing = 0;
        held->selection_explicit = 0;
        if (voice_menu_rows(held, "menu", &rows, &count, NULL) || !count || strcmp(rows[0].action, "menu:sessions"))
            fail("confirmation is primary");
        voice_menu_rows_free(rows, count);
        held->selection_explicit = 1;
        held->pending = 1;
        rows = NULL;
        if (voice_menu_rows(held, "menu", &rows, &count, NULL)
            || !row_text(rows, count, "recover-stage")
            || strncmp(row_text(rows, count, "recover-stage"), "Stage in same label", 19)
            || strcmp(row_text(rows, count, "discard"), "Discard waiting dictation")
            || strcmp(row_text(rows, count, "recover-discard"), "Discard unrecovered dictation")
            || has_row(rows, count, "stop")) fail("recovery wording");
        voice_menu_rows_free(rows, count);
        {
            VoiceMenuStatus *empty = voice_menu_status_new();
            if (voice_menu_prompt(empty, "menu", &prompt, NULL) || strcmp(prompt, "Voice | No session"))
                fail("missing session prompt");
            voice_menu_status_free(empty);
        }
        free(prompt);
        prompt = NULL;
        {
            VoiceMenuStatus *failed = status_new();
            free(failed->phase);
            failed->phase = strdup("error");
            failed->error = strdup("secret microphone fault");
            if (voice_menu_prompt(failed, "menu", &prompt, NULL) || !strstr(prompt, "Unavailable")
                || strstr(prompt, "secret")) fail("prompt hides error detail");
            free(prompt);
            voice_menu_status_free(failed);
        }
        {
            VoiceMenuStatus *state = status_new();
            free(state->pane);
            state->pane = NULL;
            state->sessions[0].selected = 0;
            state->sessions[1].selected = 0;
            free(state->sessions[0].label);
            state->sessions[0].label = strdup("alpha workspace alpha workspace alpha workspace alpha workspace alpha workspace alpha workspace · pane-9");
            free(state->sessions[1].label);
            state->sessions[1].label = strdup(state->sessions[0].label);
            rows = NULL;
            if (voice_menu_rows(state, "sessions", &rows, &count, NULL) || count != 2
                || strcmp(rows[0].action, "select:a") || strcmp(rows[1].action, "select:b")
                || !strcmp(rows[0].label, rows[1].label) || !strstr(rows[0].label, "pane-9")
                || !strstr(rows[0].label, "…") || !strstr(rows[1].label, "thread-")) fail("stable identity suffix");
            voice_menu_rows_free(rows, count);
            voice_menu_status_free(state);
        }
        sample[0].action = "select:a";
        sample[0].label = "same\nlabel";
        sample[1].action = "select:b";
        sample[1].label = "same\tlabel";
        if (voice_menu_fuzzel_plan("Sessions", sample, 2, "/config", &argv, &input, NULL)
            || !argv[4] || strcmp(argv[4], "--index") || !argv[5] || strcmp(argv[5], "--only-match")
            || strcmp(input, "same label\nsame label\n")) fail("fuzzel plan sanitizes");
        if (voice_menu_interpret_fuzzel(0, "1\n", "", 2, &action, sample, NULL) || strcmp(action, "select:b"))
            fail("fuzzel index");
        free(action);
        action = NULL;
        if (voice_menu_interpret_fuzzel(0, "voice:data", "", 1, &action, sample, &error) == 0) fail("reject custom output");
        free(error);
        error = NULL;
        if (voice_menu_interpret_fuzzel(1, "", "", 1, &action, sample, &error) != 1) fail("escape is cancel");
        if (voice_menu_interpret_fuzzel(1, "", "bad configuration", 1, &action, sample, &error) == 0
            || !error || !strstr(error, "Fuzzel")) fail("picker failure");
        free(error);
        free(argv[0]); free(argv[1]); free(argv[2]); free(argv[3]); free(argv[4]); free(argv[5]);
        free(argv[6]); free(argv[7]); free(argv[8]); free(argv[9]); free(argv[10]); free(argv[11]);
        free(argv);
        free(input);
        voice_menu_status_free(held);
    }
    {
        VoiceMenuStatus *first = status_new();
        VoiceMenuStatus *second = status_new();
        Script script = { first, second, 0, NULL };
        Picker picker = { "menu:voices", NULL, 0 };
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, NULL, NULL, NULL)
            || script.calls != 2 || script.first || script.second || strcmp(script.last, "status")) fail("submenu frees transferred status");
        free(script.last);
        free(picker.prompt);
    }
    {
        VoiceMenuStatus *busy = status_new();
        Script script = { busy, NULL, 0, NULL };
        Picker picker = { "conversation:start", NULL, 0 };
        VoiceMenuConversation ops;
        char *error = NULL;
        memset(&ops, 0, sizeof ops);
        ops.active = NULL;
        busy->conversation_available = 1;
        free(busy->phase);
        busy->phase = strdup("recording");
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, &ops, NULL, &error) == 0
            || !error || !strstr(error, "Invalid voice menu action") || script.first) fail("personaplex busy blocks switch");
        free(error);
        free(script.last);
        free(picker.prompt);
    }
    {
        VoiceMenuStatus *held = status_new();
        Script script = { held, NULL, 0, NULL };
        Picker picker = { NULL, NULL, 0 };
        VoiceMenuConversation ops;
        char *error = NULL;
        memset(&ops, 0, sizeof ops);
        ops.active = personaplex_active;
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, &ops, NULL, &error) == 0
            || !error || !strstr(error, "PersonaPlex menu is unavailable") || script.calls || !script.first)
            fail("personaplex busy menu");
        free(error);
        voice_menu_status_free(script.first);
        free(picker.prompt);
    }
    {
        VoiceMenuStatus *ready = status_new();
        VoiceMenuStatus *fresh = status_new();
        Script script = { ready, fresh, 0, NULL };
        Picker picker = { "conversation:start", NULL, 0 };
        VoiceMenuConversation ops;
        char *error = NULL;
        memset(&ops, 0, sizeof ops);
        ops.start = personaplex_start;
        ready->conversation_available = 1;
        free(fresh->phase);
        fresh->phase = strdup("recording");
        start_calls = 0;
        if (voice_menu_run("menu", script_request, &script, script_pick, &picker, &ops, NULL, &error) == 0
            || !error || !strstr(error, "Finish or discard") || start_calls || script.first || script.second)
            fail("personaplex readiness failure");
        free(error);
        free(script.last);
        free(picker.prompt);
    }
    {
        VoiceMenuStatus *queued = status_new();
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        queued->draft = 1;
        if (voice_menu_rows(queued, "menu", &rows, &count, NULL) || !count || strcmp(rows[0].action, "append")
            || strcmp(rows[0].label, "Record more") || has_row(rows, count, "stop")) fail("queued send is record more");
        voice_menu_rows_free(rows, count);
        queued->draft = 0;
        queued->retained = 1;
        queued->selection_explicit_missing = 0;
        queued->selection_explicit = 0;
        rows = NULL;
        if (voice_menu_rows(queued, "menu", &rows, &count, NULL) || strcmp(rows[0].action, "menu:sessions")) fail("retained confirmation is primary");
        voice_menu_rows_free(rows, count);
        queued->retained = 0;
        free(queued->phase);
        queued->phase = strdup("recording");
        rows = NULL;
        if (voice_menu_rows(queued, "menu", &rows, &count, NULL) || strcmp(rows[0].action, "stop")
            || strcmp(rows[0].label, "Cancel recording")) fail("stop is primary while recording");
        voice_menu_rows_free(rows, count);
        voice_menu_status_free(queued);
    }
    {
        VoiceMenuStatus *many = voice_menu_status_new();
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        size_t i;
        char key[16];
        char label[64];
        char action[32];
        char cjk[40 * 3 + 1];
        char *input = NULL;
        char **argv = NULL;
        many->pane = strdup("pane-a");
        many->harness = strdup("pi");
        many->phase = strdup("idle");
        for (i = 0; i < 70; i++) {
            snprintf(key, sizeof key, "s%zu", i);
            snprintf(label, sizeof label, "session %zu", i);
            if (voice_menu_add_session(many, key, key, label, NULL, NULL, NULL, i == 69, 0)) fail("session population");
            snprintf(key, sizeof key, "v%zu", i);
            snprintf(label, sizeof label, "voice %zu", i);
            if (voice_menu_add_named(&many->voices, &many->voice_count, key, label)) fail("voice population");
            snprintf(key, sizeof key, "t%zu", i);
            snprintf(label, sizeof label, "stt %zu", i);
            if (voice_menu_add_named(&many->stt_backends, &many->stt_count, key, label)) fail("stt population");
            snprintf(key, sizeof key, "p%zu", i);
            snprintf(label, sizeof label, "speech %zu", i);
            if (voice_menu_add_named(&many->speech_backends, &many->speech_count, key, label)) fail("speech population");
        }
        if (voice_menu_rows(many, "sessions", &rows, &count, NULL) || count != 70 || !has_row(rows, count, "select:s0")
            || !has_row(rows, count, "select:s69")) fail("sessions are not capped at 64");
        voice_menu_rows_free(rows, count);
        rows = NULL;
        if (voice_menu_rows(many, "voices", &rows, &count, NULL) || count != 70 || !has_row(rows, count, "voice:v69")) fail("voices are not capped at 64");
        voice_menu_rows_free(rows, count);
        rows = NULL;
        if (voice_menu_rows(many, "dictation", &rows, &count, NULL) || count != 70 || !has_row(rows, count, "stt:t69")) fail("dictation is not capped at 64");
        voice_menu_rows_free(rows, count);
        rows = NULL;
        if (voice_menu_rows(many, "speech", &rows, &count, NULL) || count != 70 || !has_row(rows, count, "speech:p69")) fail("speech is not capped at 64");
        voice_menu_rows_free(rows, count);
        cjk[0] = 0;
        for (i = 0; i < 40; i++) memcpy(cjk + i * 3, "\xe4\xbc\x9a", 3);
        cjk[120] = 0;
        snprintf(action, sizeof action, "select:s0");
        rows = NULL;
        free(many->sessions[0].label);
        many->sessions[0].label = strdup(cjk);
        if (voice_menu_rows(many, "sessions", &rows, &count, NULL) || !row_text(rows, count, "select:s0")
            || !strstr(row_text(rows, count, "select:s0"), cjk)) fail("long unicode session label");
        {
            VoiceMenuRow sample[2];
            sample[0].action = "select:s0";
            sample[0].label = (char *)row_text(rows, count, "select:s0");
            sample[1].action = "select:s1";
            sample[1].label = (char *)cjk;
            if (voice_menu_fuzzel_plan("Voice", sample, 2, "/config", &argv, &input, NULL) || !input
                || !strstr(input, cjk) || strlen(input) < 240) fail("fuzzel keeps unicode labels");
            for (i = 0; argv && argv[i]; i++) free(argv[i]);
            free(argv);
            free(input);
        }
        voice_menu_rows_free(rows, count);
        voice_menu_status_free(many);
    }
    return failures;
}
