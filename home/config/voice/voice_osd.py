"""Floating dictation card at the top of the focused monitor.

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
}


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
        if key in ("background", "text", "border") and value.startswith("#"):
            colours[key] = value
        elif key == "border" and value.startswith("#"):
            colours["accent"] = value
    if colours["border"].startswith("#"):
        colours["accent"] = colours["border"]
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


def osd_view(status):
    """Public card state. Transcript and reply fields are ignored."""
    if not isinstance(status, dict):
        status = {}
    phase = status.get("phase")
    phase = phase if isinstance(phase, str) else "idle"
    message = status.get("osd_message")
    message = public_label(message) if isinstance(message, str) else ""
    visible = phase in ACTIVE or bool(status.get("osd"))
    titles = {
        "starting": "Starting microphone",
        "recording": "Listening",
        "stopping": "Finishing",
        "transcribing": "Transcribing",
        "error": "Voice unavailable",
        "draft": "Dictation ready",
        "idle": "Voice",
    }
    title = titles.get(phase, "Voice")
    if phase == "recording":
        elapsed = status.get("recording_seconds") or 0
        try:
            elapsed = max(0, int(elapsed))
        except (TypeError, ValueError):
            elapsed = 0
        title = f"Listening {elapsed // 60:02d}:{elapsed % 60:02d}"
    detail = destination(status, phase)
    if message and phase not in ACTIVE:
        title = "Voice unavailable"
        detail = message
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
        "level": level,
        "meter": meter_blocks([level] if phase == "recording" else []),
        "recording": phase == "recording",
        "transcribing": phase == "transcribing",
    }


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
    import gi

    gi.require_version("Gdk", "4.0")
    gi.require_version("Gtk", "4.0")
    gi.require_version("Gtk4LayerShell", "1.0")
    gi.require_version("Pango", "1.0")
    from gi.repository import Gdk, GLib, Gtk, Gtk4LayerShell, Pango

    colours = popup_colours(
        theme_path().read_text() if theme_path().is_file() else ""
    )
    app = Gtk.Application(application_id="uk.hues.pi-voice-osd")
    state = {"connector": None, "levels": [0.0] * METER_CELLS}

    def apply_monitor():
        connector = focused_connector(read_json(["hyprctl", "monitors", "-j"]))
        if connector == state["connector"]:
            return
        state["connector"] = connector
        display = Gdk.Display.get_default()
        if display is None or not connector:
            return
        monitors = display.get_monitors()
        for index in range(monitors.get_n_items()):
            monitor = monitors.get_item(index)
            if monitor.get_connector() == connector:
                Gtk4LayerShell.set_monitor(window, monitor)
                return

    def tick():
        view = osd_view(read_status() or {})
        if not view["visible"]:
            window.set_visible(False)
            return True
        if not window.get_visible():
            window.present()
        window.set_visible(True)
        apply_monitor()
        title.set_label(view["title"])
        detail.set_label(view["detail"])
        if view["recording"]:
            state["levels"] = state["levels"][1:] + [view["level"]]
        elif not view["transcribing"]:
            state["levels"] = [0.0] * METER_CELLS
        meter.set_label(meter_blocks(state["levels"]))
        return True

    def activate(_app):
        global window, title, detail, meter
        window = Gtk.ApplicationWindow(application=app)
        window.set_decorated(False)
        window.set_resizable(False)
        Gtk4LayerShell.init_for_window(window)
        Gtk4LayerShell.set_layer(window, Gtk4LayerShell.Layer.OVERLAY)
        Gtk4LayerShell.set_namespace(window, "pi-voice-osd")
        Gtk4LayerShell.set_keyboard_mode(
            window, Gtk4LayerShell.KeyboardMode.NONE
        )
        Gtk4LayerShell.set_exclusive_zone(window, 0)
        Gtk4LayerShell.set_anchor(window, Gtk4LayerShell.Edge.TOP, True)
        Gtk4LayerShell.set_margin(window, Gtk4LayerShell.Edge.TOP, 18)
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        card.add_css_class("card")
        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=18)
        title = Gtk.Label(label="Voice", xalign=0)
        title.set_halign(Gtk.Align.START)
        meter = Gtk.Label(label=meter_blocks([]), xalign=1)
        meter.add_css_class("meter")
        meter.set_halign(Gtk.Align.END)
        meter.set_hexpand(True)
        header.append(title)
        header.append(meter)
        detail = Gtk.Label(label="", xalign=0)
        detail.add_css_class("detail")
        detail.set_halign(Gtk.Align.START)
        detail.set_ellipsize(Pango.EllipsizeMode.END)
        detail.set_max_width_chars(46)
        card.append(header)
        card.append(detail)
        window.set_child(card)
        style = Gtk.CssProvider()
        style.load_from_data(
            f"""
            window {{
              background: transparent;
            }}
            .card {{
              background: alpha({colours["background"]}, 0.96);
              color: {colours["text"]};
              border: 2px solid {colours["border"]};
              border-radius: 16px;
              padding: 12px 16px 13px;
            }}
            label {{
              color: {colours["text"]};
              font-size: 15px;
            }}
            label.title {{
              font-weight: 650;
            }}
            label.detail {{
              font-size: 13px;
              opacity: 0.78;
            }}
            label.meter {{
              color: {colours["accent"]};
              font-family: monospace;
              font-size: 18px;
              letter-spacing: 1px;
            }}
            """.encode(),
            -1,
        )
        title.add_css_class("title")
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            style,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
        )
        window.set_visible(False)
        GLib.timeout_add(80, tick)

    app.connect("activate", activate)
    return app.run([sys.argv[0]])


if __name__ == "__main__":
    raise SystemExit(run())
