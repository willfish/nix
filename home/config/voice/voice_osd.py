"""Floating dictation pill at the top of the focused monitor.

Shows phase, the selected Pi session and microphone level only. Never renders
dictated text or assistant replies.
"""

import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import time


ACTIVE = {"starting", "recording", "stopping", "transcribing"}
METER_CELLS = 8
METER_FLOOR_DB = -50
METER_CEILING_DB = -10
PEAK_BLOCKS = ("▁", "▂", "▃", "▄", "▅", "▆", "▇", "█")
THINKING = {"off", "minimal", "low", "medium", "high", "xhigh", "max"}
FLAGS = {"selected", "team", "pi", "qwen-pi", "qwen pi"}
DEFAULT_COLOURS = {
    "background": "#222222",
    "text": "#c2c2b0",
    "border": "#78824b",
    "accent": "#c9a554",
    "muted": "#8a8a78",
    "red": "#e0303e",
    "yellow": "#c9a554",
    "green": "#2aa45f",
    "orange": "#e2681e",
    "teal": "#2f9e9a",
}
TONES = ("red", "yellow", "green", "orange", "teal", "accent", "muted")


def public_label(value, limit=120):
    text = " ".join(str(value or "").split())
    return "".join(char for char in text if char.isprintable())[:limit]


def focused_connector(monitors):
    """Return the single focused Hyprland connector, or None."""
    if not isinstance(monitors, list):
        return None
    focused = [
        row for row in monitors
        if isinstance(row, dict) and row.get("focused") is True
    ]
    if len(focused) != 1:
        return None
    name = focused[0].get("name")
    return name if isinstance(name, str) and name else None


def popup_colours(text):
    """Read the active theme popup colours. Unknown files keep the defaults."""
    colours = dict(DEFAULT_COLOURS)
    section = None
    if not isinstance(text, str):
        return colours
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            continue
        if section != "popups" or "=" not in line:
            continue
        key, value = (part.strip().strip("\"'") for part in line.split("=", 1))
        if key in colours and value.startswith("#") and len(value) in (4, 7, 9):
            colours[key] = value
    accent_is_default = colours["accent"] == DEFAULT_COLOURS["accent"]
    if colours["border"].startswith("#") and accent_is_default:
        colours["accent"] = colours["border"]
    return colours


def waybar_colours(text):
    """Theme roles already published for the bar."""
    colours = {}
    if not isinstance(text, str):
        return colours
    for raw in text.splitlines():
        line = raw.strip().rstrip(";")
        if not line.startswith("@define-color "):
            continue
        parts = line.split()
        if len(parts) < 3 or parts[1] not in DEFAULT_COLOURS:
            continue
        value = parts[2]
        if value.startswith("#") and len(value) in (4, 7, 9):
            colours[parts[1]] = value
    return colours


def resolved_colours(shell_text, waybar_text=""):
    """Waybar fills theme roles. Explicit popup keys still win."""
    colours = dict(DEFAULT_COLOURS)
    colours.update(waybar_colours(waybar_text))
    explicit = set()
    section = None
    if isinstance(shell_text, str):
        for raw in shell_text.splitlines():
            line = raw.strip()
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip()
                continue
            if section == "popups" and "=" in line and not line.startswith("#"):
                explicit.add(line.split("=", 1)[0].strip())
    parsed = popup_colours(shell_text)
    for key in explicit:
        if key in parsed:
            colours[key] = parsed[key]
    if "border" in explicit and "accent" not in explicit:
        colours["accent"] = parsed["border"]
    return colours


def fit_label(text, limit=46):
    text = public_label(text, 200)
    if len(text) <= limit:
        return text
    return text[: limit - 1].rstrip() + "…"


def compact_destination(text):
    """Keep the workspace and tab. Drop harness, model and selection flags."""
    parts = []
    for part in str(text or "").split(" · "):
        piece = part.strip()
        lowered = piece.lower()
        if not piece or lowered in THINKING or lowered in FLAGS:
            continue
        if piece.startswith("@") or ":" in piece:
            continue
        if any(char.isdigit() for char in piece) and (
            "-" in piece or "." in piece
        ):
            continue
        parts.append(piece)
    return fit_label(" · ".join(parts))


def level_to_block(level):
    """Peak cells for the dictation card meter."""
    try:
        level = float(level or 0)
    except (TypeError, ValueError):
        level = 0.0
    if not level > 0:
        return PEAK_BLOCKS[0]
    db = 20 * math.log10(min(1.0, level))
    span = METER_CEILING_DB - METER_FLOOR_DB
    scale = max(0.0, min(1.0, (db - METER_FLOOR_DB) / span))
    index = min(len(PEAK_BLOCKS) - 1, int(scale * (len(PEAK_BLOCKS) - 1)))
    return PEAK_BLOCKS[index]


def meter_blocks(levels):
    cells = list(levels or [])[-METER_CELLS:]
    cells = [0.0] * (METER_CELLS - len(cells)) + cells
    return "".join(level_to_block(level) for level in cells)


def destination(status, phase):
    rows = status.get("sessions")
    rows = rows if isinstance(rows, list) else []
    selected = next(
        (row for row in rows if isinstance(row, dict) and row.get("selected")),
        {},
    )
    label = None
    if phase in ACTIVE:
        label = status.get("recording_label")
    label = (
        label
        or selected.get("full_label")
        or selected.get("label")
        or status.get("session_label")
    )
    if not label and not status.get("pane"):
        return "No Pi session selected"
    return compact_destination(
        label or status.get("pane") or "Selected Pi session"
    ) or "Selected Pi session"


def card_state(status, phase, message):
    """Choose one public title. Dictation outranks speech and attention."""
    connection = status.get("connection_state")
    if phase in ACTIVE:
        titles = {
            "starting": "Starting microphone",
            "stopping": "Finishing",
            "transcribing": "Transcribing",
        }
        title = titles.get(phase, "Listening")
        if phase == "recording":
            title = "Listening " + recording_timer(
                status.get("recording_seconds")
            )
        return title, "red" if phase == "recording" else "yellow"
    if status.get("osd") and message:
        tone = status.get("osd_tone")
        return message, tone if tone in TONES else "orange"
    if phase == "error":
        return public_label(status.get("error") or "Voice unavailable"), "red"
    if status.get("queued"):
        return "Will send when idle", "green"
    if status.get("pending") or (
        status.get("draft") and not status.get("draft_edited")
    ):
        return "Ready to send", "green"
    if status.get("retained"):
        return "Dictation retained", "orange"
    if status.get("speaking"):
        if status.get("audible"):
            return "Speaking", "teal"
        return "About to speak", "accent"
    if status.get("agent_state") == "blocked":
        return "Needs attention", "orange"
    if connection in ("connecting", "reconnecting"):
        return connection.capitalize(), "yellow"
    if status.get("rebind_needed"):
        return "Conversation changed", "yellow"
    if status.get("models") == "unavailable":
        return "Speech models unavailable", "orange"
    if status.get("osd"):
        return "Voice", "accent"
    return "Voice", "muted"


def osd_view(status):
    """Public card state. Transcript and reply fields are ignored."""
    if not isinstance(status, dict):
        status = {}
    phase = status.get("phase")
    phase = phase if isinstance(phase, str) else "idle"
    message = status.get("osd_message")
    message = public_label(message) if isinstance(message, str) else ""
    title, tone = card_state(status, phase, message)
    visible = phase in ACTIVE or bool(status.get("osd")) or title != "Voice"
    if status.get("draft_edited") and title == "Voice" and not message:
        visible = False
    detail = destination(status, phase)
    if status.get("retained") and phase not in ACTIVE:
        source = public_label(status.get("retained_source") or "")
        if source:
            detail = fit_label(f"From {source}")
    microphone = status.get("microphone") if isinstance(
        status.get("microphone"), dict
    ) else {}
    if phase == "recording" and microphone:
        warnings = []
        if microphone.get("muted"):
            warnings.append("muted")
        if microphone.get("clipping"):
            warnings.append("clipping")
        if warnings:
            detail = fit_label(f"{detail} · {', '.join(warnings)}")
    if (
        status.get("models") == "unavailable"
        and title == "Speech models unavailable"
        and status.get("model_error")
    ):
        detail = public_label(status.get("model_error"), 80) or detail
    level = status.get("input_level") if phase == "recording" else 0
    try:
        level = float(level or 0)
    except (TypeError, ValueError):
        level = 0.0
    level = min(1.0, max(0.0, level))
    return {
        "visible": visible,
        "title": title,
        "detail": detail,
        "tone": tone if tone in TONES else "muted",
        "level": level,
        "meter": meter_blocks([level] if phase == "recording" else []),
        "recording": phase == "recording",
        "transcribing": phase == "transcribing",
        "phase": phase,
        "seconds": finite_seconds(status.get("recording_seconds")),
        "timer": recording_timer(status.get("recording_seconds")),
        "muted": bool(microphone.get("muted")),
        "clipping": bool(microphone.get("clipping")),
    }


def finite_seconds(value):
    try:
        return max(0, int(float(value or 0)))
    except (TypeError, ValueError, OverflowError):
        return 0


def recording_timer(value):
    seconds = finite_seconds(value)
    return f"{seconds // 60:02d}:{seconds % 60:02d}"


def command_env():
    env = os.environ.copy()
    # Layer-shell preload must not leak into hyprctl or pi-voice.
    env.pop("LD_PRELOAD", None)
    return env


def status_from_response(payload):
    if not isinstance(payload, dict) or payload.get("ok") is not True:
        return None
    return {
        key: value for key, value in payload.items() if key != "ok"
    }


def notice_path():
    runtime = os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}")
    return Path(runtime) / "pi-voice" / "osd-notice.json"


def read_notice():
    """External voice message used only when the controller card is hidden."""
    try:
        payload = json.loads(notice_path().read_text())
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict):
        return None
    try:
        if float(payload.get("until") or 0) <= time.time():
            return None
    except (TypeError, ValueError):
        return None
    message = payload.get("message")
    if not isinstance(message, str) or not message.strip():
        return None
    tone = payload.get("tone")
    return osd_view({
        "osd": True,
        "osd_message": message,
        "osd_tone": tone if tone in TONES else "red",
    })


def read_status():
    runtime = os.environ.get(
        "XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"
    )
    path = Path(runtime) / "pi-voice" / "control.sock"
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.4)
            sock.connect(str(path))
            sock.sendall(b'{"action":"status"}\n')
            with sock.makefile("rb") as incoming:
                raw = incoming.readline(1024 * 1024)
        return status_from_response(json.loads(raw))
    except (OSError, json.JSONDecodeError, TimeoutError):
        return None


def read_json(command):
    try:
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=1,
            env=command_env(),
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0 or not result.stdout.strip():
        return None
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return None


def theme_path():
    state = os.environ.get("XDG_STATE_HOME")
    root = Path(state) if state else Path.home() / ".local/state"
    return root / "theme-menu/active/shell.toml"


def run():
    if not os.environ.get("WAYLAND_DISPLAY"):
        print("pi-voice-osd: no Wayland display", file=sys.stderr)
        return 0
    from voice_pill import run_pill

    shell = theme_path().read_text() if theme_path().is_file() else ""
    waybar = theme_path().with_name("waybar.css")
    colours = resolved_colours(
        shell, waybar.read_text() if waybar.is_file() else ""
    )
    return run_pill(
        osd_view, read_status, read_notice, read_json,
        focused_connector, colours
    )


if __name__ == "__main__":
    raise SystemExit(run())
