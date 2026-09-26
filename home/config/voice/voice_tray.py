"""Public voice-menu presentation.

The status card, not a tray icon, shows state.
"""

import asyncio
import sys
import threading
import time


def public_label(value, limit=120):
    text = " ".join(str(value).split())
    return "".join(char for char in text if char.isprintable())[:limit]


def session_visible(row, show_team):
    """Filter presentation only, never change the selected destination."""
    return bool(row.get("selected") or show_team or not row.get("team_child"))


def selected_session(status):
    return [(row.get("token"), row.get("id"))
            for row in status.get("sessions", []) if row.get("selected")]


def presentation(status):
    """Return only public state, never dictated text or assistant replies."""
    phase = status.get("phase", "idle")
    preparing = bool(status.get("preparing_transcription"))
    busy = preparing or phase in (
        "starting", "recording", "stopping", "transcribing"
    )
    selected = bool(status.get("pane"))
    ready = bool(status.get("draft") or status.get("pending"))
    pending = bool(status.get("pending"))
    retry = bool(status.get("retry"))
    speaking = bool(status.get("speaking"))
    responding = bool(status.get("responding")) or (
        status.get("agent_state") == "working"
    )
    blocked = status.get("agent_state") == "blocked"
    connection = status.get(
        "connection_state", "ready" if selected else "unselected")
    connected = selected and connection == "ready"
    retained = bool(status.get("retained"))
    usable = connected and not blocked and not retained
    harness = public_label(status.get("harness") or "Pi", 30)
    harness = {"pi": "Pi", "qwen-pi": "Qwen Pi"}.get(harness, harness)
    glyph = "microphone"
    label, colour = {
        "idle": (
            "Ready to record" if selected else "No voice session selected",
            "grey",
        ),
        "starting": ("Starting microphone", "amber"),
        "recording": ("Recording", "red"),
        "stopping": ("Finishing recording", "amber"),
        "transcribing": ("Transcribing locally", "amber"),
        "draft": ("Dictation ready to send", "green"),
        "error": ("Voice error; try recording again", "orange"),
    }.get(phase, ("Voice unavailable", "orange"))
    if phase == "recording":
        elapsed = max(0, int(status.get("recording_seconds", 0)))
        label += f" {elapsed // 60:02d}:{elapsed % 60:02d}"
        level = min(100, max(0, int(status.get("input_level", 0) * 100)))
        label += f" (input {level}%)"
    elif phase == "error" and status.get("error"):
        detail = public_label(status["error"])
        label = f"Voice error: {detail}"
    if phase == "error":
        glyph = "blocked"
    elif not busy:
        if ready:
            label = f"Dictation waiting for {harness}" if pending else (
                "Dictation ready; Super+Space sends it"
            )
            colour, glyph = "green", "check"
        elif speaking:
            label, colour, glyph = "Reading reply aloud", "blue", "speaker"
        elif selected and blocked:
            label = f"{harness} needs your attention"
            colour, glyph = "orange", "blocked"
        elif selected and responding:
            label, colour, glyph = f"{harness} is responding", "blue", "dots"
    if preparing:
        label = "Preparing transcription"
        colour = "amber"
    elif (
        not busy
        and not speaking
        and phase != "error"
        and connection in ("connecting", "reconnecting")
    ):
        label, colour = connection.capitalize(), "amber"
    elif not selected and not busy and not speaking and phase != "error":
        label = "No voice session selected"
    context = []
    selected_row = next((row for row in status.get("sessions", [])
                         if row.get("selected")), {})
    destination = (
        status.get("recording_label") if busy else None
    ) or selected_row.get("full_label") or status.get("session_label")
    if (
        selected
        or (busy and status.get("recording_label"))
        or connection == "reconnecting"
    ):
        session = public_label(destination or status.get(
            "pane") or "Previous destination", None)
        context.append(f"{harness}: {session}")
        if connection != "ready":
            context.append(connection.capitalize())
        if responding and glyph != "dots":
            context.append(f"{harness} is responding")
        elif blocked and label != f"{harness} needs your attention":
            context.append(f"{harness} needs your attention")
    microphone = status.get("microphone") or {}
    if microphone:
        name = public_label(microphone.get("name") or "Unknown")
        mic_label = f"Microphone: {name}"
        for key, message in (
            ("muted", "muted"), ("clipping", "clipping; lower input volume"),
            ("missing", "preferred microphone unavailable; using default"),
        ):
            if microphone.get(key):
                mic_label += f" ({message})"
        context.append(mic_label)
        if microphone.get("error"):
            context.append(public_label(microphone["error"]))
    models = status.get("models")
    if models == "loading":
        context.append("Speech models loading")
        if colour == "grey":
            colour = "amber"
    elif models == "unavailable":
        context.append("Speech models unavailable")
        if status.get("model_error"):
            context.append(public_label(status["model_error"]))
        if colour in ("grey", "amber") and not busy:
            colour = "orange"
    if status.get("rebind_needed"):
        context.append(
            "Conversation changed; choose Bind to current conversation"
        )
        if phase == "idle" and colour == "grey":
            colour = "amber"
    view = {
        "label": label,
        "colour": colour,
        "glyph": glyph,
        "context": context,
        "auto": bool(status.get("auto")),
        "selected_voice": status.get("selected_voice", "samantha"),
        "selected_stt": status.get("selected_stt") or "whisper",
        "selected_session": None,
        "selection_identity": selected_session(status),
        "session_identities": {},
        "full_labels": {},
        "show_team": bool(status.get("show_team", False)),
        "actions": {
            "auto-toggle": ("Read replies aloud", True),
            "team-toggle": ("Show team members", True),
        },
    }
    actions = view["actions"]
    can_speak = status.get('can_speak', True)
    if selected and not can_speak:
        context.append('Team members are silent')
    for character, label in status.get("voices", {}).items():
        actions["voice:" + character] = (public_label(label), True)
    selected_stt = status.get("selected_stt") or "whisper"
    for name, label in status.get("stt_backends", {}).items():
        actions["stt:" + name] = (public_label(label), not busy)
    context.append(
        "Dictation: Deepgram" if selected_stt == "deepgram"
        else "Dictation: Whisper"
    )
    if connected and not blocked and not busy and not pending and not speaking \
            and not responding and can_speak and status.get("reply"):
        actions["read"] = ("Replay last reply", True)
    if (
        busy
        or speaking
        or retained
        or connection in ("connecting", "reconnecting")
        or status.get("models") == "loading"
    ):
        actions["stop"] = (
            "Cancel transcription" if phase == "transcribing" or preparing
            else "Cancel recording" if busy else "Stop speaking" if speaking
            else "Stop", True,
        )
    if usable and ready and not busy:
        actions["append"] = ("Record more", True)
    if usable and retry and not busy:
        actions["retry"] = ("Retry transcription", True)
    if (pending or retry) and not busy:
        actions["discard"] = (
            "Discard retained dictation" if pending
            else "Discard retained recording", True,
        )
    if selected and status.get("rebind_needed") and not busy:
        actions["rebind"] = ("Bind to current conversation", True)
    if retained:
        source = public_label(status.get("retained_source")
                              or "Previous destination")
        context.append(f"Retained dictation from {source}")
        actions["recover-copy"] = ("Copy retained dictation", True)
        actions["recover-stage"] = (
            "Stage in selected Pi session",
            connected and status.get("harness") in ("pi", "qwen-pi")
            and status.get("selection_explicit", True)
            and not busy and not blocked and not responding,
        )
        actions["recover-discard"] = ("Discard retained dictation", True)
    for session in status.get("sessions", []):
        if not session_visible(session, view["show_team"]):
            continue
        token = session.get("token")
        if not isinstance(token, str) or not token:
            continue
        is_selected = bool(session.get("selected"))
        if is_selected:
            view["selected_session"] = f"select:{token}"
        view["session_identities"][f"select:{token}"] = (
            token, session.get("id"))
        view["full_labels"][f"select:{token}"] = public_label(
            session.get("full_label") or session.get("label") or token, None
        )
        confirm = (is_selected and retained
                   and status.get("harness") in ("pi", "qwen-pi")
                   and not status.get("selection_explicit", True))
        label = public_label(session.get("label") or token)
        view["actions"][f"select:{token}"] = (
            f"Confirm {label} for retained dictation" if confirm else label,
            not busy and (not is_selected or confirm)
            and session.get("connection_state", "ready") == "ready",
        )
    return view
