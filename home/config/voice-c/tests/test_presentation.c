#include "presentation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

static VoiceMenuStatus *base_status(void) {
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

static int action_enabled(const VoicePresentation *view, const char *action) {
    const VoiceAction *found = voice_presentation_action(view, action);
    return found && found->enabled;
}

int test_presentation(void) {
    failures = 0;
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        voice_menu_add_session(status, "a", "1", NULL, NULL, NULL, NULL, 1, 1);
        voice_menu_add_session(status, "b", "2", NULL, NULL, NULL, NULL, 0, 1);
        voice_menu_add_session(status, "c", "3", "short", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", NULL, NULL, 0, 0);
        if (voice_presentation(status, &view, NULL) || view->show_team
            || !voice_presentation_action(view, "select:a")
            || voice_presentation_action(view, "select:b")
            || !view->full_labels[0].label || strlen(view->full_labels[view->full_label_count - 1].label) != 200) fail("team filter full labels");
        voice_presentation_free(view);
        status->show_team = 1;
        if (voice_presentation(status, &view, NULL) || !voice_presentation_action(view, "select:b")) fail("show team reveals child");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        const char *harnesses[] = {"pi", "qwen-pi", "unknown", "pi"};
        const char *connections[] = {"ready", "ready", "ready", "reconnecting"};
        int allowed[] = {1, 1, 0, 0};
        size_t i;
        for (i = 0; i < 4; i++) {
            VoiceMenuStatus *status = voice_menu_status_new();
            VoicePresentation *view;
            status->retained = 1;
            status->retained_source = strdup("pi source");
            status->pane = strdup("p1");
            status->harness = strdup(harnesses[i]);
            status->connection_state = strdup(connections[i]);
            if (voice_presentation(status, &view, NULL)
                || action_enabled(view, "recover-stage") != allowed[i]
                || !voice_presentation_action(view, "recover-copy")
                || !voice_presentation_action(view, "recover-discard")
                || !voice_presentation_action(view, "stop")
                || voice_presentation_action(view, "send")
                || voice_presentation_action(view, "append")
                || !voice_presentation_contains(view, "pi source")) fail("recovery stage gate");
            voice_presentation_free(view);
            voice_menu_status_free(status);
        }
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        const VoiceAction *select;
        status->pane = strdup("p1");
        status->harness = strdup("pi");
        status->retained = 1;
        status->selection_explicit_missing = 0;
        status->selection_explicit = 0;
        voice_menu_add_session(status, "one", "conversation", "Pi: notes", NULL, NULL, NULL, 1, 0);
        if (voice_presentation(status, &view, NULL)) fail("confirm present");
        select = voice_presentation_action(view, "select:one");
        if (!select || strcmp(select->label, "Confirm Pi: notes for retained dictation") || !select->enabled
            || action_enabled(view, "recover-stage")) fail("retained current requires confirmation");
        voice_presentation_free(view);
        status->selection_explicit = 1;
        if (voice_presentation(status, &view, NULL) || action_enabled(view, "select:one")
            || !action_enabled(view, "recover-stage")) fail("explicit selection can stage");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        status->pane = strdup("p1");
        status->connection_state = strdup("reconnecting");
        status->draft = 1;
        if (voice_presentation(status, &view, NULL) || strcmp(view->label, "Reconnecting")
            || voice_presentation_action(view, "append") || !action_enabled(view, "stop")) fail("reconnecting stop");
        voice_presentation_free(view);
        voice_menu_status_free(status);
        status = voice_menu_status_new();
        status->pane = strdup("new");
        status->session_label = strdup("new");
        status->recording_label = strdup("pinned");
        status->phase = strdup("recording");
        status->preparing = 1;
        if (voice_presentation(status, &view, NULL) || strcmp(view->label, "Preparing transcription")
            || !voice_presentation_contains(view, "pinned") || !action_enabled(view, "stop")) fail("preparing keeps pin");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        status->pane = strdup("p1");
        status->phase = strdup("recording");
        status->has_recording_seconds = 1;
        status->recording_seconds = 65.9;
        status->has_input_level = 1;
        status->input_level = 0.8;
        status->draft = 1;
        if (voice_presentation(status, &view, NULL) || !strstr(view->label, "01:05") || strcmp(view->colour, "red")
            || voice_presentation_action(view, "send") || voice_presentation_action(view, "read")
            || !voice_presentation_action(view, "stop") || strcmp(voice_presentation_action(view, "stop")->label, "Cancel recording"))
            fail("recording timer");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        char raw[256];
        memset(raw, 'x', 200);
        memcpy(raw, "Microphone disconnected\n\x1b", 26);
        raw[226] = 0;
        status->phase = strdup("error");
        status->error = strdup(raw);
        if (voice_presentation(status, &view, NULL) || !strstr(view->label, "Microphone disconnected")
            || strchr(view->label, '\n') || strchr(view->label, '\x1b') || strlen(view->label) > 150) fail("bounded error");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        const char *json =
            "{\"phase\":\"error\",\"error\":\"Microphone failed\",\"reply\":\"private answer\","
            "\"transcript\":\"private request\",\"draft\":\"secret dictation\",\"pane\":\"p1\"}";
        VoiceMenuStatus *status = NULL;
        VoicePresentation *view = NULL;
        if (voice_menu_status_from_json(json, &status, NULL) || voice_menu_status_contains(status, "private")
            || voice_menu_status_contains(status, "secret") || !status->has_reply || !status->draft
            || voice_presentation(status, &view, NULL) || voice_presentation_contains(view, "private")
            || voice_presentation_contains(view, "secret")) fail("private text dropped");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        status->pane = strdup("p1");
        status->harness = strdup("Pi");
        status->session_label = strdup("dotfiles");
        status->models = strdup("loading");
        status->microphone.present = 1;
        status->microphone.name = strdup("USB microphone");
        status->microphone.muted = 1;
        status->microphone.missing = 1;
        if (voice_presentation(status, &view, NULL) || !voice_presentation_contains(view, "Pi: dotfiles")
            || !voice_presentation_contains(view, "USB microphone")
            || !voice_presentation_contains(view, "muted")
            || !voice_presentation_contains(view, "preferred microphone unavailable")
            || !voice_presentation_contains(view, "Speech models loading")
            || strcmp(view->colour, "amber")) fail("context harness mic models");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        status->pane = strdup("p1");
        status->speech_available_missing = 0;
        if (voice_presentation(status, &view, NULL) || voice_presentation_action(view, "auto-toggle")
            || !voice_presentation_action(view, "team-toggle")) fail("speech toggle hidden");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        voice_menu_add_named(&status->stt_backends, &status->stt_count, "whisper", "Whisper (local GPU)");
        voice_menu_add_named(&status->stt_backends, &status->stt_count, "deepgram", "Deepgram (cloud)");
        status->selected_stt = strdup("whisper");
        if (voice_presentation(status, &view, NULL) || !action_enabled(view, "stt:whisper")
            || !action_enabled(view, "stt:deepgram") || !voice_presentation_contains(view, "Dictation: Whisper"))
            fail("dictation choices");
        voice_presentation_free(view);
        status->phase = strdup("recording");
        free(status->selected_stt);
        status->selected_stt = strdup("deepgram");
        if (voice_presentation(status, &view, NULL) || action_enabled(view, "stt:whisper")
            || action_enabled(view, "stt:deepgram") || !voice_presentation_contains(view, "Dictation: Deepgram"))
            fail("dictation locks while busy");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = base_status();
        VoicePresentation *view;
        status->has_reply = 1;
        if (voice_presentation(status, &view, NULL)
            || !voice_presentation_action(view, "read")) fail("replay when usable");
        voice_presentation_free(view);
        status->pending = 1;
        if (voice_presentation(status, &view, NULL) || voice_presentation_action(view, "read")) fail("pending hides replay");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    {
        VoiceMenuStatus *status = voice_menu_status_new();
        VoicePresentation *view;
        voice_menu_add_named(&status->speech_backends, &status->speech_count, "local", "Local");
        voice_menu_add_named(&status->speech_backends, &status->speech_count, "deepgram", "Deepgram");
        if (voice_presentation(status, &view, NULL) || !voice_presentation_action(view, "speech:local")
            || !voice_presentation_action(view, "speech:deepgram")) fail("speech backends");
        voice_presentation_free(view);
        voice_menu_status_free(status);
    }
    return failures;
}
