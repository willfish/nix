#include "osd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

static void join_osd(const OsdView *view, char *out, size_t cap) {
    snprintf(out, cap, "%d %s %s %s %.6f %s %d %d %s %.6f %s %d %d",
        view->visible, view->title, view->detail, view->tone, view->level, view->meter,
        view->recording, view->transcribing, view->phase, view->seconds, view->timer,
        view->muted, view->clipping);
}

static size_t codepoints(const char *text) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
}

static VoiceStatus blank(void) {
    VoiceStatus status;
    memset(&status, 0, sizeof status);
    return status;
}

int test_osd(void) {
    OsdView view;
    char rendered[1024];
    failures = 0;

    {
        VoiceStatus status = blank();
        status.phase = "idle";
        status.pane = "w1:p1";
        osd_view(&status, &view);
        if (view.visible) fail("hidden until shown");
    }
    {
        VoiceSession sessions[] = {{.selected = 1, .full_label = "pi \xc2\xb7 notes \xc2\xb7 selected"}};
        VoiceStatus status = blank();
        status.phase = "idle";
        status.osd = 1;
        status.pane = "w1:p1";
        status.sessions = sessions;
        status.session_count = 1;
        osd_view(&status, &view);
        if (!view.visible || strcmp(view.detail, "notes") != 0) fail("shown destination");
    }

    {
        VoiceStatus status = blank();
        status.phase = "recording";
        status.has_recording_seconds = 1;
        status.recording_seconds = 65;
        status.has_input_level = 1;
        status.input_level = 1.4;
        status.recording_label = "pi \xc2\xb7 tariff";
        status.pending = 1;
        osd_view(&status, &view);
        join_osd(&view, rendered, sizeof rendered);
        if (!view.visible || strcmp(view.title, "Listening 01:05") != 0
            || strcmp(view.detail, "tariff") != 0 || view.level != 1.0
            || strcmp(view.meter + strlen(view.meter) - 3, "\xe2\x96\x88") != 0
            || strstr(rendered, "secret")) {
            fail("recording names the selected session and level");
        }
    }

    {
        VoiceStatus status = blank();
        status.phase = "idle";
        status.osd = 1;
        status.osd_message = "Select a Pi voice session first";
        status.pending = 1;
        osd_view(&status, &view);
        join_osd(&view, rendered, sizeof rendered);
        if (strcmp(view.title, "Select a Pi voice session first") != 0
            || strcmp(view.tone, "orange") != 0
            || strcmp(view.detail, "do not show this") == 0
            || strstr(rendered, "do not show")) {
            fail("failure shows the reason not a transcript");
        }
    }
    {
        VoiceStatus status = blank();
        status.phase = "idle";
        status.osd = 1;
        status.osd_message = "Hello\xe2\x80\x8b\xe2\x80\xaeWorld\xc2\xa0Next";
        osd_view(&status, &view);
        if (strcmp(view.title, "HelloWorld Next") != 0) fail("public label strips format and unicode space");
    }

    {
        char meter[64];
        VoiceStatus status = blank();
        status.phase = "recording";
        status.recording_label = "pi \xc2\xb7 dot \xc2\xb7 5 dictation like omarchy \xc2\xb7 medium \xc2\xb7 grok-4.7 \xc2\xb7 selected";
        osd_view(&status, &view);
        meter_blocks((double[]){0.4}, 1, meter, sizeof meter);
        if (strcmp(view.detail, "dot \xc2\xb7 5 dictation like omarchy") != 0
            || strcmp(level_to_block(0), "\xe2\x96\x81") != 0
            || strcmp(level_to_block(1), "\xe2\x96\x88") != 0
            || codepoints(meter) != 8) {
            fail("session line drops model and selection flags");
        }
        meter_blocks((double[]){1}, 1, meter, sizeof meter);
        if (!meter[0] || strcmp(meter + strlen(meter) - 3, "\xe2\x96\x88") != 0) {
            fail("meter ends with full block");
        }
    }

    {
        VoiceStatus status = blank();
        status.phase = "starting";
        status.osd = 1;
        osd_view(&status, &view);
        if (strcmp(view.detail, "No Pi session selected") != 0) fail("no session is explicit");
    }

    {
        VoiceMonitor one[] = {
            {.name = "DP-1", .focused = 1},
            {.name = "HDMI-A-1", .focused = 0},
        };
        VoiceMonitor two[] = {
            {.name = "DP-1", .focused = 1},
            {.name = "HDMI-A-1", .focused = 1},
        };
        const char *name = focused_connector(one, 2);
        if (!name || strcmp(name, "DP-1") != 0 || focused_connector(two, 2) != NULL) {
            fail("focused connector requires one focused monitor");
        }
    }

    {
        VoiceStatus status;
        VoiceStatusPayload ok = {.ok_is_true = 1, .phase = "recording", .reply = "secret reply"};
        VoiceStatusPayload bad = {.ok_is_true = 0, .phase = "recording", .reply = "secret reply"};
        if (!status_from_response(&ok, &status) || !status.phase || strcmp(status.phase, "recording") != 0
            || status_from_response(&bad, &status)) {
            fail("status socket drops the ok flag and keeps public fields");
        }
        osd_view(&status, &view);
        join_osd(&view, rendered, sizeof rendered);
        if (strstr(rendered, "secret")) fail("status reply is not a view field");
    }

    {
        VoiceColours colours;
        popup_colours("[popups]\nbackground = \"#111111\"\ntext = \"#eeeeee\"\nborder = \"#abcdef\"\n", &colours);
        if (strcmp(colours.background, "#111111") != 0 || strcmp(colours.text, "#eeeeee") != 0
            || strcmp(colours.accent, "#abcdef") != 0) {
            fail("popup colours follow the active theme");
        }
    }

    {
        VoiceColours colours;
        popup_colours("[popups]\nred = \"#aa0000\"\nteal = \"#00aaaa\"\naccent = \"#0000aa\"\n", &colours);
        if (strcmp(colours.red, "#aa0000") != 0 || strcmp(colours.teal, "#00aaaa") != 0
            || strcmp(colours.accent, "#0000aa") != 0) {
            fail("popup colours keep explicit state roles");
        }
    }

    {
        VoiceColours colours;
        resolved_colours("[popups]\nborder = \"#111111\"\n",
            "@define-color red #aa0000;\n@define-color teal #00aaaa;\n@define-color accent #0000ff;\n",
            &colours);
        if (strcmp(colours.red, "#aa0000") != 0 || strcmp(colours.teal, "#00aaaa") != 0
            || strcmp(colours.accent, "#111111") != 0) {
            fail("waybar roles fill colours the popup file omits");
        }
    }

    {
        VoiceStatus hidden = blank();
        hidden.phase = "idle";
        hidden.pane = "w1:p1";
        osd_view(&hidden, &view);
        if (view.visible) fail("working agent stays hidden");
        VoiceSession sessions[] = {{.selected = 1, .full_label = "pi \xc2\xb7 notes \xc2\xb7 selected"}};
        VoiceStatus imminent = blank();
        imminent.phase = "idle";
        imminent.pane = "w1:p1";
        imminent.speaking = 1;
        imminent.sessions = sessions;
        imminent.session_count = 1;
        osd_view(&imminent, &view);
        if (!view.visible || strcmp(view.title, "About to speak") != 0
            || strcmp(view.tone, "accent") != 0 || strcmp(view.detail, "notes") != 0) {
            fail("about to speak");
        }
        VoiceStatus speaking = blank();
        speaking.phase = "idle";
        speaking.pane = "w1:p1";
        speaking.speaking = 1;
        speaking.audible = 1;
        osd_view(&speaking, &view);
        if (strcmp(view.title, "Speaking") != 0 || strcmp(view.tone, "teal") != 0) {
            fail("speaking tone");
        }
    }

    {
        VoiceStatus status = blank();
        status.phase = "recording";
        status.speaking = 1;
        status.audible = 1;
        status.has_recording_seconds = 1;
        status.recording_seconds = 3;
        osd_view(&status, &view);
        if (strncmp(view.title, "Listening", 9) != 0 || strcmp(view.tone, "red") != 0) {
            fail("dictation outranks speech");
        }
        VoiceStatus ready = blank();
        ready.phase = "draft";
        ready.draft = 1;
        ready.pane = "w1:p1";
        osd_view(&ready, &view);
        join_osd(&view, rendered, sizeof rendered);
        if (strcmp(view.title, "Ready to send") != 0 || strcmp(view.tone, "green") != 0
            || strstr(rendered, "secret")) {
            fail("ready uses green");
        }
    }

    for (int draft = 1; draft >= 0; draft--) {
        VoiceStatus status = blank();
        status.phase = draft ? "draft" : "idle";
        status.draft = draft;
        status.draft_edited = 1;
        status.osd = 1;
        osd_view(&status, &view);
        if (view.visible) fail("editor changes hide ready to send");
    }
    {
        VoiceStatus pending = blank();
        pending.draft_edited = 1;
        pending.pending = 1;
        osd_view(&pending, &view);
        if (strcmp(view.title, "Ready to send") != 0) fail("pending still ready");
        VoiceStatus notice = blank();
        notice.draft_edited = 1;
        notice.osd = 1;
        notice.osd_message = "Microphone unavailable";
        osd_view(&notice, &view);
        if (!view.visible || strcmp(view.title, "Microphone unavailable") != 0) {
            fail("stale reveal keeps a real notice");
        }
    }

    {
        VoiceStatus blocked = blank();
        blocked.phase = "idle";
        blocked.pane = "w1:p1";
        blocked.agent_state = "blocked";
        osd_view(&blocked, &view);
        join_osd(&view, rendered, sizeof rendered);
        if (strcmp(view.title, "Needs attention") != 0 || strcmp(view.tone, "orange") != 0
            || strstr(rendered, "secret")) {
            fail("blocked attention");
        }
        VoiceStatus retained = blank();
        retained.retained = 1;
        retained.retained_source = "notes";
        osd_view(&retained, &view);
        if (strcmp(view.title, "Dictation retained") != 0 || strcmp(view.detail, "From notes") != 0) {
            fail("retained source");
        }
    }

    return failures;
}
