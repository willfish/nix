"""Short-lived keyboard front end for the shared voice controller."""

import argparse
from functools import partial
import subprocess
import sys

from codex_voice import call, desktop_notice
from codex_voice_tray import presentation, public_label


def rows_for(status, section):
    view = presentation(status)
    actions = view["actions"]
    if section == "voices":
        return [
            (
                action,
                ("* " if action == "voice:" + view["selected_voice"] else "")
                + label,
            )
            for action, (label, enabled) in actions.items()
            if action.startswith("voice:") and enabled
        ]
    if section == "sessions":
        return [
            (action, label)
            for action, (label, enabled) in actions.items()
            if action.startswith("select:") and enabled
        ]
    rows = [
        ("menu:sessions", "Choose session"),
        ("menu:voices", "Choose voice"),
    ]
    for action, (label, enabled) in actions.items():
        if enabled and not action.startswith(("voice:", "select:")):
            if action == "auto-toggle":
                label += ": " + ("on" if view["auto"] else "off")
            rows.append((action, label))
    return rows


def pick(prompt, rows, config):
    result = subprocess.run(
        [
            "fuzzel",
            "--config",
            config,
            "--dmenu",
            "--index",
            "--only-match",
            "--match-mode=fzf",
            "--no-mouse",
            "--log-level=error",
            "--log-no-syslog",
            "--prompt",
            public_label(prompt, 80) + "> ",
        ],
        input="\n".join(public_label(label) for _, label in rows) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        if result.stderr.strip():
            raise RuntimeError(
                "Fuzzel could not open the voice menu: "
                + public_label(result.stderr)
            )
        return None  # Escape/focus loss must never select an action.
    output = result.stdout.strip()
    if not output.isascii() or not output.isdecimal():
        raise RuntimeError("Fuzzel returned an invalid selection")
    index = int(output)
    if not 0 <= index < len(rows):
        raise RuntimeError("Fuzzel returned an invalid selection")
    return rows[index][0]


def selected_session(status):
    return [
        (s.get("token"), s.get("id"))
        for s in status.get("sessions", [])
        if s.get("selected")
    ]


def run_menu(section="menu", *, request=call, picker):
    status = request({"action": "status"})
    while True:
        rows = rows_for(status, section)
        if not rows:
            raise RuntimeError(
                "No other selectable sessions"
                if section == "sessions"
                else "No voices are available"
            )
        current = status.get("selected_voice", "samantha")
        prompt = {
            "menu": "Voice controls",
            "voices": f"Voice ({current})",
            "sessions": "Other voice sessions",
        }[section]
        action = picker(prompt, rows)
        if action is None:
            return
        if action not in dict(rows):
            raise RuntimeError("Invalid voice menu action")
        if action.startswith("menu:"):
            section = action.split(":", 1)[1]
            status = request({"action": "status"})
            continue
        fresh = request({"action": "status"})
        enabled = presentation(fresh)["actions"].get(action, ("", False))[1]
        if not enabled:
            raise RuntimeError(
                "That action is no longer available; reopen the menu"
            )
        if not action.startswith(("voice:", "select:")):
            if selected_session(fresh) != selected_session(status):
                raise RuntimeError(
                    "The selected session changed; reopen the menu"
                )
            if action == "auto-toggle" and fresh.get("auto") != status.get(
                "auto"
            ):
                raise RuntimeError(
                    "The automatic playback setting changed; reopen the menu"
                )
        request({"action": action})
        return


def main(args=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "section",
        nargs="?",
        choices=["menu", "voices", "sessions"],
        default="menu",
    )
    parser.add_argument(
        "--config", required=True, help="Dedicated Fuzzel configuration"
    )
    options = parser.parse_args(args)
    try:
        run_menu(options.section, picker=partial(pick, config=options.config))
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        detail = public_label(exc)
        print(f"voice-menu: {detail}", file=sys.stderr)
        desktop_notice("Voice menu unavailable", detail)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
