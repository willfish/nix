#define _POSIX_C_SOURCE 200809L
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void failure(const char *message) {
  fprintf(stderr, "%s\n", message);
  exit(1);
}
static gboolean utf8(const char *s, size_t n) {
  while (n) {
    const char *zero = memchr(s, 0, n);
    size_t part = zero ? (size_t)(zero - s) : n;
    if (!g_utf8_validate(s, (gssize)part, NULL))
      return FALSE;
    s += part;
    n -= part;
    if (zero) {
      s++;
      n--;
    }
  }
  return TRUE;
}
static GString *read_text(const char *path) {
  char *s = NULL;
  gsize n;
  if (!g_file_get_contents(path, &s, &n, NULL))
    failure("Could not read patch target");
  if (!utf8(s, n)) {
    g_free(s);
    failure("Invalid UTF-8 patch target");
  }
  GString *text = g_string_sized_new(n);
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\r') {
      g_string_append_c(text, '\n');
      if (i + 1 < n && s[i + 1] == '\n')
        i++;
    } else
      g_string_append_c(text, s[i]);
  }
  g_free(s);
  return text;
}
static void write_text(const char *path, GString *text) {
  FILE *file = fopen(path, "w");
  if (!file)
    failure("Could not write patch target");
  gboolean ok = fwrite(text->str, 1, text->len, file) == text->len;
  if (fclose(file) != 0)
    ok = FALSE;
  if (!ok)
    failure("Could not write patch target");
}
static void replace(GString *text, const char *old, const char *new,
                    const char *label) {
  size_t n = strlen(old);
  const char *hit = NULL;
  for (size_t i = 0; i + n <= text->len; i++)
    if (!memcmp(text->str + i, old, n)) {
      hit = text->str + i;
      break;
    }
  if (!hit) {
    char *msg = g_strconcat("missing ", label, NULL);
    failure(msg);
  }
  size_t at = (size_t)(hit - text->str);
  g_string_erase(text, (gssize)at, (gssize)n);
  g_string_insert(text, (gssize)at, new);
}
static void prepare(const char *out) {
  char *color = g_build_filename(out, "shell/Commons/Color.qml", NULL),
       *panel = g_build_filename(out, "shell/Ui/KeyboardPanel.qml", NULL);
  GString *text = read_text(color);
  replace(text,
          "readonly property string currentThemePath: stateHome + "
          "\"/omarchy/current/theme\"",
          "readonly property string currentThemePath: "
          "Quickshell.env(\"HYPR_CONTROLS_THEME\")",
          "theme path");
  replace(
      text,
      "path: root.currentThemePath + \"/colors.toml\"\n    watchChanges: false",
      "path: root.currentThemePath + \"/colors.toml\"\n    watchChanges: "
      "true\n    onFileChanged: reload()",
      "colors watch");
  replace(
      text,
      "path: root.currentThemePath + \"/shell.toml\"\n    watchChanges: false",
      "path: root.currentThemePath + \"/shell.toml\"\n    watchChanges: true\n "
      "   onFileChanged: reload()",
      "shell watch");
  replace(text,
          "path: root.home + \"/.config/omarchy/shell.toml\"\n    "
          "watchChanges: true",
          "path: \"\"\n    watchChanges: false", "user shell override");
  write_text(color, text);
  g_string_free(text, TRUE);
  text = read_text(panel);
  replace(text, "screen: anchorWindow ? anchorWindow.screen : null",
          "screen: bar.externalScreen", "panel screen");
  replace(text, "if (!anchorItem || !anchorWindow) return Qt.point(0, 0)",
          "if (!anchorWindow) return Qt.point(bar.externalAnchor.x - anchorW / "
          "2, bar.externalAnchor.y - anchorH / 2)",
          "panel anchor");
  replace(text,
          "readonly property real barW: anchorWindow ? anchorWindow.width : "
          "screenW\n  readonly property real barH: anchorWindow ? "
          "anchorWindow.height : 0",
          "readonly property real barW: bar.vertical ? bar.barSize : screenW\n "
          " readonly property real barH: bar.vertical ? screenH : bar.barSize",
          "panel bar size");
  replace(
      text,
      "mask: Region {\n    width: root.screenW\n    height: root.screenH\n  }",
      "mask: Region {\n    width: root.screenW\n    height: root.screenH\n    "
      "Region {\n      intersection: Intersection.Subtract\n      x: "
      "root.barPos === \"right\" ? root.screenW - root.bar.barSize : 0\n      "
      "y: root.barPos === \"bottom\" ? root.screenH - root.bar.barSize : 0\n   "
      "   width: root.bar.vertical ? root.bar.barSize : root.screenW\n      "
      "height: root.bar.vertical ? root.screenH : root.bar.barSize\n    }\n  }",
      "panel mask");
  write_text(panel, text);
  g_string_free(text, TRUE);
  g_free(color);
  g_free(panel);
}
static void patch_icon(const char *path) {
  GString *text = read_text(path);
  replace(text,
          "def from_icon_theme(names):\n    \"\"\"Whatever the machine already "
          "has for this name.\"\"\"\n    for name in names:",
          "def from_icon_theme(names):\n    \"\"\"Whatever the machine already "
          "has for this name.\"\"\"\n    names = "
          "icon_paths.expand_names(names)\n    for name in names:",
          "icon theme lookup");
  replace(text,
          "if want in haystack.split(\"-\") or (\"-\" + want + \"-\") in "
          "(\"-\" + haystack + \"-\"):",
          "if icon_paths.name_matches(want, haystack):", "desktop name match");
  write_text(path, text);
  g_string_free(text, TRUE);
}
#define EXEC_ARGV                                                              \
  "  function runExecArgv(argv) {\n"                                           \
  "    if (argv[0] === \"xdg-open\" || argv[0] === \"omarchy-agent-crash\")\n" \
  "      Quickshell.execDetached([\"/usr/bin/python3\", \"-I\", "              \
  "service.actionBin].concat(argv))\n"                                         \
  "    else\n"                                                                 \
  "      Quickshell.execDetached(argv[0].charAt(0) === \"/\" ? argv : "        \
  "[\"/usr/bin/env\"].concat(argv))\n"                                         \
  "  }\n"
static void patch_herdr(const char *path, const char *script) {
  GString *text = read_text(path);
  char *helper = g_strconcat(
      EXEC_ARGV,
      "\n\n  function herdrToast(row) {\n"
      "    var summary = String(row && row.summary || \"\")\n"
      "    var body = String(row && row.body || \"\")\n"
      "    var app = String(row && row.app || \"\").toLowerCase()\n"
      "    var icon = String(row && row.appIcon || \"\").toLowerCase()\n"
      "    var ghostty = summary === \"Ghostty\" || app.indexOf(\"ghostty\") "
      ">= 0 || icon.indexOf(\"ghostty\") >= 0\n"
      "    if (!ghostty) return false\n"
      "    var text = summary + \"\\n\" + body\n"
      "    return text.indexOf(\"finished:\") >= 0 || text.indexOf(\"needs "
      "attention:\") >= 0\n"
      "  }\n\n  function herdrFocus(command, row) {\n"
      "    Quickshell.execDetached([\n      \"",
      script,
      "\",\n"
      "      command,\n"
      "      \"--summary\", String(row.summary || \"\"),\n"
      "      \"--body\", String(row.body || \"\"),\n"
      "      \"--ts\", String(row.ts || \"\"),\n"
      "      \"--pid\", String(row.senderPid || \"\")\n"
      "    ])\n  }\n\n"
      "  function rememberHerdrTarget(row) {\n"
      "    if (herdrToast(row)) herdrFocus(\"remember\", row)\n  }\n\n"
      "  function openHerdrTarget(row) {\n"
      "    if (!herdrToast(row)) return false\n"
      "    herdrFocus(\"open\", row)\n"
      "    return true\n  }\n",
      NULL);
  replace(text, EXEC_ARGV, helper, "exec helper");
  g_free(helper);
  replace(
      text,
      "    var row = Store.snapshot(notification, key, NotificationUrgency)\n  "
      "  row.duration = durationFor(notification.urgency, row.expireTimeout)\n "
      "   rememberRecent(row)\n",
      "    var row = Store.snapshot(notification, key, NotificationUrgency)\n  "
      "  row.duration = durationFor(notification.urgency, row.expireTimeout)\n "
      "   rememberRecent(row)\n    rememberHerdrTarget(row)\n",
      "remember call");
  replace(text,
          "    if (!handled) {\n      if (row) {\n        // Source first, "
          "link last. A Slack message quoting a link to\n",
          "    if (!handled && row && openHerdrTarget(row)) {\n      // The "
          "helper focuses the Herdr pane before raising Ghostty.\n    } else "
          "if (!handled) {\n      if (row) {\n        // Source first, link "
          "last. A Slack message quoting a link to\n",
          "activate branch");
  write_text(path, text);
  g_string_free(text, TRUE);
}
int main(int argc, char **argv) {
  if (!strcmp(TOOL_NAME, "omapager-patch-herdr-focus")) {
    if (argc != 3)
      failure("usage: omapager-patch-herdr-focus SERVICE_QML FOCUS_BIN");
    patch_herdr(argv[1], argv[2]);
  } else {
    if (argc < 2)
      failure("Patch target required");
    if (!strcmp(TOOL_NAME, "omapager-prepare-shell"))
      prepare(argv[1]);
    else
      patch_icon(argv[1]);
  }
  return 0;
}
