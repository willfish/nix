"""Short-lived keyboard front end for the shared voice controller."""

import argparse
import os
from functools import partial
import subprocess
import sys

from voice_controller import call, desktop_notice
from voice_conversation import Conversation, can_switch
from voice_tray import presentation, public_label, selected_session


LIVE_PHASES = ("starting", "recording", "stopping", "transcribing")
MENU_LAUNCHERS = {
    "menu:sessions": "Choose session",
    "menu:voices": "Choose voice",
    "menu:dictation": "Choose dictation",
    "menu:speech": "Choose speech",
}


def voice_busy(status):
    return bool(status.get("preparing_transcription")) or status.get(
        "phase", "idle"
    ) in LIVE_PHASES


def clip_label(value, limit=50):
    """Fit a row without dropping a trailing workspace, tab, or pane mark."""
    text = public_label(value, None)
    if len(text) <= limit:
        return text
    if " · " in text:
        head, tail = text.rsplit(" · ", 1)
        room = limit - len(tail) - 3
        if room >= 8:
            return head[:room - 1].rstrip() + "… · " + tail
    return text[:limit - 1].rstrip() + "…"


def _identity_source(identity, action):
    token, session_id = "", ""
    if isinstance(identity, tuple):
        token = "" if not identity else identity[0] or ""
        session_id = "" if len(identity) < 2 else identity[1] or ""
    return public_label(session_id or token or action, None) or "session"


def collision_suffixes(actions, identities):
    sources = [
        _identity_source(identities.get(action), action) for action in actions
    ]
    size = 4
    longest = max(len(source) for source in sources)
    while size < longest:
        cuts = [source[:size] for source in sources]
        if len(set(cuts)) == len(cuts):
            return dict(zip(actions, cuts))
        size += 1
    return {
        action: source[:size] for action, source in zip(actions, sources)
    }


def present_labels(pairs, identities):
    """pairs are (action, label, marked). Dispatch actions stay unchanged."""
    clipped = [
        (action, clip_label(label, 50), marked)
        for action, label, marked in pairs
    ]
    counts = {}
    for _, label, _ in clipped:
        counts[label] = counts.get(label, 0) + 1
    colliding = [action for action, label, _ in clipped if counts[label] > 1]
    suffixes = collision_suffixes(colliding, identities) if colliding else {}
    shown = []
    for action, label, marked in clipped:
        suffix = suffixes.get(action)
        if suffix:
            label = clip_label(label, max(8, 50 - len(suffix) - 3))
            label = f"{label} · {suffix}"
        if marked:
            label = "* " + label
        shown.append((action, label))
    return shown


def session_pairs(status, view):
    actions = view["actions"]
    selected_child = {
        "select:" + row["token"]
        for row in status.get("sessions", [])
        if row.get("selected") and row.get("team_child")
        and isinstance(row.get("token"), str)
    }
    selected = view.get("selected_session")
    pairs = []
    for action, (label, enabled) in actions.items():
        if not action.startswith("select:"):
            continue
        if not enabled and action not in selected_child:
            continue
        pairs.append((
            action, label, action in selected_child or action == selected
        ))
    return present_labels(pairs, view.get("session_identities") or {})


def prefixed_rows(actions, prefix, marked_action=None):
    pairs = [
        (action, label, action == marked_action)
        for action, (label, enabled) in actions.items()
        if action.startswith(prefix) and enabled
    ]
    return present_labels(pairs, {})


def needs_destination_confirmation(status):
    return (
        bool(status.get("retained"))
        and status.get("harness") in ("pi", "qwen-pi")
        and not status.get("selection_explicit", True)
        and bool(status.get("pane"))
    )


def live_stop(status):
    return voice_busy(status) or bool(status.get("speaking")) or (
        status.get("connection_state") in ("connecting", "reconnecting")
    ) or status.get("models") == "loading"


def root_order(status):
    order = ["stop"] if live_stop(status) else []
    order.extend((
        "append",
        "retry",
        "read",
        "rebind",
        "recover-stage",
        "recover-copy",
        "menu:sessions",
        "menu:speech",
        "menu:voices",
        "menu:dictation",
        "auto-toggle",
        "team-toggle",
    ))
    if not live_stop(status):
        order.append("stop")
    order.extend(("discard", "recover-discard", "conversation:start"))
    return order


def phase_primary(status, present):
    if live_stop(status) and "stop" in present:
        return "stop"
    if needs_destination_confirmation(status) and "menu:sessions" in present:
        return "menu:sessions"
    for action in ("recover-stage", "append", "read"):
        if action in present:
            return action
    return None


def order_root(status, rows):
    present = {action: label for action, label in rows}
    primary = phase_primary(status, present)
    known = set(root_order(status))
    ordered = []
    if primary:
        ordered.append((primary, present[primary]))
    for action in root_order(status):
        if action != primary and action in present:
            ordered.append((action, present[action]))
    extras = [
        (action, label) for action, label in rows
        if action not in known and action != primary
    ]
    if extras and ordered and ordered[-1][0] == "conversation:start":
        ordered[-1:-1] = extras
    else:
        ordered.extend(extras)
    return ordered


def menu_label(status, action, label):
    """Menu wording only. Pill and shared presentation labels stay unchanged."""
    if action == "discard" and status.get("pending"):
        return "Discard waiting dictation"
    if action == "recover-discard":
        return "Discard unrecovered dictation"
    if action == "recover-stage":
        destination = next(
            (
                row.get("label")
                for row in status.get("sessions", [])
                if row.get("selected") and row.get("label")
            ),
            status.get("session_label") or "selected Pi session",
        )
        return "Stage in " + public_label(destination, 40)
    return label


def rows_for(status, section):
    view = presentation(status)
    actions = view["actions"]
    if section == "voices":
        return prefixed_rows(
            actions, "voice:", "voice:" + view["selected_voice"]
        )
    if section == "sessions":
        return session_pairs(status, view)
    if section == "speech":
        return prefixed_rows(
            actions, "speech:",
            "speech:" + status.get("speech_backend", "local")
        )
    if section == "dictation":
        return prefixed_rows(
            actions, "stt:", "stt:" + (status.get("selected_stt") or "whisper")
        )
    rows = []
    submenu_rows = {
        "menu:sessions": session_pairs(status, view),
        "menu:voices": prefixed_rows(actions, "voice:"),
        "menu:dictation": prefixed_rows(actions, "stt:"),
        "menu:speech": prefixed_rows(actions, "speech:"),
    }
    for action, title in MENU_LAUNCHERS.items():
        if submenu_rows[action]:
            rows.append((action, title))
    for action, (label, enabled) in actions.items():
        if not enabled or action.startswith(
            ("voice:", "select:", "stt:", "speech:")
        ):
            continue
        if action == "stop" and not live_stop(status):
            continue
        toggle = {"auto-toggle": "auto", "team-toggle": "show_team"}.get(
            action
        )
        if toggle:
            label += ": " + ("on" if view[toggle] else "off")
        rows.append((action, clip_label(menu_label(status, action, label), 52)))
    if status.get("conversation_available") and can_switch(status):
        rows.append((
            "conversation:start",
            "Try PersonaPlex conversation (experimental)",
        ))
    return order_root(status, rows)


def empty_menu_reason(status, section):
    if section == "sessions":
        if voice_busy(status):
            return "Session switching is locked until this take finishes"
        return "No other selectable sessions"
    if section == "dictation":
        if status.get("stt_backends") and voice_busy(status):
            return "Dictation is locked until this take finishes"
        return "No dictation backends are available"
    return "No voices are available"


def fit_prompt(text, limit=28):
    """Leave room to type, keeping a trailing pane or tab mark."""
    text = public_label(text, None)
    if len(text) <= limit:
        return text
    if " \u00b7 " in text:
        head, tail = text.rsplit(" \u00b7 ", 1)
        room = limit - len(tail) - 3
        if 8 <= room < len(head):
            return head[:room - 1].rstrip() + "\u2026 \u00b7 " + tail
    return text[:limit - 1].rstrip() + "\u2026"


def prompt_for(status, section):
    current = status.get("selected_voice", "samantha")
    current_stt = status.get("selected_stt") or "whisper"
    if section != "menu":
        return {
            "voices": f"Voice ({current})",
            "dictation": f"Dictation ({current_stt})",
            "speech": "Speech backend",
            "sessions": "Sessions",
        }[section]
    if not (
        status.get("pane")
        or status.get("connection_state") == "reconnecting"
        or (voice_busy(status) and status.get("recording_label"))
    ):
        return "Voice | No session"
    if status.get("phase") == "error":
        return "Voice | Unavailable"
    if status.get("retained"):
        return fit_prompt(
            "From " + public_label(
                status.get("retained_source") or "previous destination", None
            )
        )
    destination = status.get("recording_label") if voice_busy(status) else None
    destination = destination or status.get("session_label")
    if not destination:
        return "Voice"
    return fit_prompt("Voice | " + public_label(destination, None))


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


def run_menu(section="menu", *, request=call, picker, conversation=None):
    # Never call the Pi controller while PersonaPlex owns audio: call() starts
    # Pi's service, which intentionally conflicts with PersonaPlex.
    if conversation is not None:
        if conversation.active():
            conversation.menu(picker)
            return
        original_request = request

        def request(message):
            result = original_request(message)
            if message.get("action") == "status":
                result = {**result, "conversation_available": True}
            return result

    status = request({"action": "status"})
    while True:
        view = presentation(status)
        rows = rows_for(status, section)
        if not rows:
            raise RuntimeError(empty_menu_reason(status, section))
        prompt = prompt_for(status, section)
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
        if action == "conversation:start":
            if conversation is None or not can_switch(fresh):
                raise RuntimeError(
                    "Finish or discard dictation before switching voice modes"
                )
            conversation.start()
            return
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
        choices=["menu", "voices", "dictation", "speech", "sessions"],
        default="menu",
    )
    parser.add_argument(
        "--config", required=True, help="Dedicated Fuzzel configuration"
    )
    options = parser.parse_args(args)
    try:
        run_menu(
            options.section, picker=partial(pick, config=options.config),
            conversation=(
                Conversation()
                if os.environ.get("PI_PERSONAPLEX_ENABLED") == "1" else None
            ),
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        detail = public_label(exc)
        print(f"voice-menu: {detail}", file=sys.stderr)
        desktop_notice("Voice menu unavailable", detail)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
