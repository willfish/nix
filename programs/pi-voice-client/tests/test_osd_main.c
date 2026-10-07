#include "osd_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

int test_osd_main(void) {
    failures = 0;
    {
        const char *json =
            "{\"ok\":true,\"phase\":\"recording\",\"reply\":\"secret reply\",\"transcript\":\"secret transcript\","
            "\"draft\":\"secret draft\",\"pane\":\"p1\",\"recording_seconds\":3,\"input_level\":0.2,"
            "\"sessions\":[{\"selected\":true,\"full_label\":\"pi · dot · 1 · selected\",\"label\":\"dot\"}]}";
        OsdStatusDoc *doc = osd_status_parse(json);
        OsdView view;
        if (!doc || osd_status_contains(doc, "secret") || !osd_status_view(doc)->recording_seconds) fail("status drops private text");
        osd_view(osd_status_view(doc), &view);
        if (!view.recording || strstr(view.title, "secret") || strstr(view.detail, "secret")) fail("render has no transcript");
        osd_status_free(doc);
        if (osd_status_parse("{\"phase\":\"recording\"}")) fail("missing ok is not status");
    }
    {
        OsdView notice;
        PillView pill;
        int was = 0;
        WarningFilter filter;
        warning_filter_init(&filter);
        if (osd_notice_view("{\"message\":\"Saved\",\"tone\":\"nope\",\"until\":100}", 10, &notice) != 1
            || strcmp(notice.tone, "red") || strcmp(notice.title, "Saved")) fail("notice tone fallback");
        if (osd_notice_view("{\"message\":\"Saved\",\"tone\":\"green\",\"until\":10}", 10, &notice)) fail("expired notice");
        osd_apply_sample(1, NULL, &was, &filter, 1, "{\"message\":\"Held\",\"tone\":\"orange\",\"until\":50}", 10, &pill);
        if (!pill.visible || strcmp(pill.title, "Held")) fail("notice overrides hidden card");
        was = 1;
        osd_apply_sample(0, NULL, &was, &filter, 2, NULL, 10, &pill);
        if (!was || !strstr(pill.title, "Voice connection lost")) fail("recording loss is unavailable");
        was = 0;
        osd_apply_sample(0, NULL, &was, &filter, 3, NULL, 10, &pill);
        if (pill.visible) fail("idle loss stays hidden");
    }
    {
        PillView current;
        PillView replaced;
        memset(&current, 0, sizeof current);
        current.recording = 1;
        if (!osd_apply_stale(&current, 0, 2.1, &replaced) || !strstr(replaced.title, "connection lost")) fail("stale recording");
        current.recording = 0;
        if (!osd_apply_stale(&current, 0, 3, &replaced) || replaced.visible) fail("stale idle resets");
        if (osd_apply_stale(&current, 0, 2, &replaced)) fail("two seconds is still fresh");
    }
    {
        VoiceMonitor *monitors = NULL;
        char **names = NULL;
        size_t count = 0;
        const char *json = "[{\"name\":\"DP-1\",\"focused\":true},{\"name\":\"HDMI-A-1\",\"focused\":1}]";
        if (osd_monitors_parse(json, &monitors, &names, &count) || count != 2
            || !focused_connector(monitors, count) || strcmp(focused_connector(monitors, count), "DP-1"))
            fail("focused connector is boolean");
        osd_monitors_free(monitors, names, count);
    }
    {
        char shell[256], waybar[256];
        PillMotion motion;
        PillView view;
        char label[128];
        osd_theme_paths("/state", "/home/will", shell, sizeof shell, waybar, sizeof waybar);
        if (strcmp(shell, "/state/theme-menu/active/shell.toml")
            || strcmp(waybar, "/state/theme-menu/active/waybar.css")) fail("theme paths");
        osd_theme_paths("", "/home/will", shell, sizeof shell, waybar, sizeof waybar);
        if (!strstr(shell, "/home/will/.local/state/theme-menu/active/shell.toml")) fail("default theme path");
        memset(&view, 0, sizeof view);
        pill_motion_init(&motion);
        if (osd_frame_continues(&view, &motion, 1, 0)) fail("idle does not animate");
        view.visible = 1;
        view.recording = 1;
        if (!osd_frame_continues(&view, &motion, 0, 1)) fail("recording animates");
        view.recording = 0;
        view.working = 1;
        if (osd_frame_continues(&view, &motion, 1, 1) && motion.width == view.width) fail("reduced work settles");
        osd_accessible_label(&view, label, sizeof label);
        if (strcmp(label, "Voice. ")) fail("accessible label");
        snprintf(view.title, sizeof view.title, "Recording");
        snprintf(view.detail, sizeof view.detail, "dot \xc2\xb7 1");
        osd_accessible_label(&view, label, sizeof label);
        if (strcmp(label, "Recording. dot \xc2\xb7 1")) fail("accessible label");
        view.visible = 0;
        osd_accessible_label(&view, label, sizeof label);
        if (strcmp(label, "Voice idle")) fail("idle accessible label");
    }
    return failures;
}
