#!/usr/bin/env python3
"""Teach Omapager to open the Herdr pane behind a Ghostty toast."""

import sys
from pathlib import Path

ROW = "    var row = Store.snapshot(notification, key, NotificationUrgency)\n"
DURATION = (
    "    row.duration = durationFor(notification.urgency, row.expireTimeout)\n"
)
REMEMBER_OLD = ROW + DURATION + "    rememberRecent(row)\n"
REMEMBER_NEW = REMEMBER_OLD + "    rememberHerdrTarget(row)\n"

ACTIVATE_OLD = """    if (!handled) {
      if (row) {
        // Source first, link last. A Slack message quoting a link to
"""

ACTIVATE_NEW = """    if (!handled && row && openHerdrTarget(row)) {
      // The helper focuses the Herdr pane before raising Ghostty.
    } else if (!handled) {
      if (row) {
        // Source first, link last. A Slack message quoting a link to
"""

EXEC_ARGV = (
    '    Quickshell.execDetached(argv[0].charAt(0) === "/" ? argv : '
    '["/usr/bin/env"].concat(argv))\n'
)
HELPER_ANCHOR = "  function runExecArgv(argv) {\n" + EXEC_ARGV + "  }\n"


def helper(script):
    exec_line = EXEC_ARGV
    ghostty = (
        '    var ghostty = summary === "Ghostty" || '
        'app.indexOf("ghostty") >= 0 || icon.indexOf("ghostty") >= 0\n'
    )
    finished = (
        '    return text.indexOf("finished:") >= 0 || '
        'text.indexOf("needs attention:") >= 0\n'
    )
    return f"""  function runExecArgv(argv) {{
{exec_line}  }}

  function herdrToast(row) {{
    var summary = String(row && row.summary || "")
    var body = String(row && row.body || "")
    var app = String(row && row.app || "").toLowerCase()
    var icon = String(row && row.appIcon || "").toLowerCase()
{ghostty}    if (!ghostty) return false
    var text = summary + "\\n" + body
{finished}  }}

  function herdrFocus(command, row) {{
    Quickshell.execDetached([
      "{script}",
      command,
      "--summary", String(row.summary || ""),
      "--body", String(row.body || ""),
      "--ts", String(row.ts || ""),
      "--pid", String(row.senderPid || "")
    ])
  }}

  function rememberHerdrTarget(row) {{
    if (herdrToast(row)) herdrFocus("remember", row)
  }}

  function openHerdrTarget(row) {{
    if (!herdrToast(row)) return false
    herdrFocus("open", row)
    return true
  }}
"""


def patch_text(text, script):
    for old, new, label in (
        (HELPER_ANCHOR, helper(script), "exec helper"),
        (REMEMBER_OLD, REMEMBER_NEW, "remember call"),
        (ACTIVATE_OLD, ACTIVATE_NEW, "activate branch"),
    ):
        if old not in text:
            raise SystemExit(f"missing {label}")
        text = text.replace(old, new, 1)
    return text


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch-herdr-focus.py SERVICE_QML FOCUS_BIN")
    path = Path(sys.argv[1])
    path.write_text(patch_text(path.read_text(), sys.argv[2]))


if __name__ == "__main__":
    main()
