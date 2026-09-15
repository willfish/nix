"""Short-lived keyboard front end for the shared voice controller."""

import argparse
from functools import partial
import subprocess
import sys

from voice_controller import call, desktop_notice
from voice_tray import presentation, public_label, selected_session


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
        selected_child = {
            "select:" + row["token"]
            for row in status.get("sessions", [])
            if row.get("selected") and row.get("team_child")
            and isinstance(row.get("token"), str)
        }
        return [
            (action, ("* " if action in selected_child else "") + label)
            for action, (label, enabled) in actions.items()
            if action.startswith("select:")
            and (enabled or action in selected_child)
        ]
    if section == "dictation":
        current_stt = status.get("selected_stt", "whisper")
        return [
            (
                action,
                ("* " if action == "stt:" + current_stt else "") + label,
            )
            for action, (label, enabled) in actions.items()
            if action.startswith("stt:") and enabled
        ]
    rows = [
        ("menu:sessions", "Choose session"),
        ("menu:voices", "Choose voice"),
        ("menu:dictation", "Choose dictation"),
    ]
    for action, (label, enabled) in actions.items():
        if enabled and not action.startswith(
            ("voice:", "select:", "stt:")
        ):
            toggle = {"auto-toggle": "auto",
                "team-toggle": "show_team"}.get(action)
            if toggle:
                label += ": " + ("on" if view[toggle] else "off")
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
            public_label(prompt, 320) + "> ",
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


def run_menu(section="menu", *, request=call, picker):
    status = request({"action": "status"})
    while True:
        rows = rows_for(status, section)
        if not rows:
            raise RuntimeError(
                "No other selectable sessions"
                if section == "sessions"
                else "No dictation backends are available"
                if section == "dictation"
                else "No voices are available"
            )
        current = status.get("selected_voice", "samantha")
        current_stt = status.get("selected_stt", "whisper")
        prompt = {
            "menu": "Voice controls",
            "voices": f"Voice ({current})",
            "dictation": f"Dictation ({current_stt})",
            "sessions": "Voice sessions",
        }[section]
        view = presentation(status)
        destination = status.get("recording_label") if status.get("phase") in (
            "starting", "recording", "stopping", "transcribing"
        ) or status.get("preparing_transcription") else None
        destination = destination or status.get("session_label")
        if destination and (
            status.get("pane")
            or status.get("connection_state") == "reconnecting"
        ):
            prompt += f" | {public_label(destination)} | {view['label']}"
        else:
            prompt += f" | {view['label']}"
        if status.get("retained"):
            prompt += " | Retained from " + public_label(
                status.get("retained_source") or "Previous destination"
            )
        action = picker(prompt, rows)
        if action is None:
            return
        if action not in dict(rows):
            raise RuntimeError("Invalid voice menu action")
        if action.startswith("menu:"):
            section = action.split(":", 1)[1]
            status = request({"action": "status"})
            continue
        if action == "stop":
            request({"action": action})
            return
        fresh = request({"action": "status"})
        fresh_view = presentation(fresh)
        enabled = fresh_view["actions"].get(action, ("", False))[1]
        if (
            action.startswith("select:")
            and action == fresh_view["selected_session"]
        ):
            if view["session_identities"].get(action) != fresh_view[
                "session_identities"
            ].get(action):
                raise RuntimeError("That session changed; reopen the menu")
            if not enabled:
                return
        if not enabled:
            raise RuntimeError(
                "That action is no longer available; reopen the menu"
            )
        if action.startswith("select:"):
            old_identity = presentation(
                status)["session_identities"].get(action)
            if old_identity != presentation(fresh)["session_identities"].get(
                action
            ):
                raise RuntimeError("That session changed; reopen the menu")
        if action in (
            "read",
            "append",
            "retry",
            "discard",
            "rebind",
            "recover-stage",
            "auto-toggle",
        ):
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
        if action == "team-toggle" and bool(fresh.get("show_team")) != bool(
            status.get("show_team")
        ):
            raise RuntimeError(
                "The team visibility setting changed; reopen the menu")
        request({"action": action})
        return


def main(args=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "section",
        nargs="?",
        choices=["menu", "voices", "dictation", "sessions"],
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
