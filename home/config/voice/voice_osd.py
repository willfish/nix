"""Floating dictation card at the top of the focused monitor.

Shows phase, the selected Pi session and microphone level only. Never renders
dictated text or assistant replies.
"""

import json
import os
from pathlib import Path
import subprocess
import sys


ACTIVE = {"starting", "recording", "stopping", "transcribing"}
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
    text = public_label(label or status.get("pane") or "Selected Pi session")
    harness = status.get("harness") or selected.get("harness") or ""
    pretty = {"pi": "Pi", "qwen-pi": "Qwen Pi"}.get(harness, "")
    if pretty and pretty.lower() not in text.lower():
        return f"{pretty}: {text}"
    return text


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
        "recording": phase == "recording",
        "transcribing": phase == "transcribing",
    }


def command_env():
    env = os.environ.copy()
    # Layer-shell preload must not leak into hyprctl or pi-voice.
    env.pop("LD_PRELOAD", None)
    return env


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
    state = {"connector": None, "pulse": 0, "levels": [0.0] * 28}

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
        view = osd_view(read_json(["pi-voice", "status"]) or {})
        if not view["visible"]:
            window.set_visible(False)
            return True
        if not window.get_visible():
            window.present()
        window.set_visible(True)
        apply_monitor()
        title.set_label(view["title"])
        detail.set_label(view["detail"])
        state["pulse"] = (state["pulse"] + 1) % len(state["levels"])
        if view["recording"]:
            state["levels"] = state["levels"][1:] + [view["level"]]
        elif view["transcribing"]:
            state["levels"] = [
                0.85 if index == state["pulse"] else 0.18
                for index in range(len(state["levels"]))
            ]
        else:
            state["levels"] = [
                max(0.0, level * 0.72) for level in state["levels"]
            ]
        for bar, level in zip(bars, state["levels"]):
            height = 4 + int(level * 28)
            bar.set_size_request(5, height)
        return True

    def activate(_app):
        global window, title, detail, bars
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
        card = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL, spacing=8
        )
        card.add_css_class("card")
        card.set_margin_top(14)
        card.set_margin_bottom(14)
        card.set_margin_start(18)
        card.set_margin_end(18)
        title = Gtk.Label(label="Voice", xalign=0)
        detail = Gtk.Label(label="", xalign=0)
        detail.set_ellipsize(Pango.EllipsizeMode.END)
        detail.set_max_width_chars(48)
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)
        row.set_valign(Gtk.Align.END)
        bars = []
        for _ in range(28):
            bar = Gtk.Box()
            bar.set_size_request(5, 4)
            bar.add_css_class("level")
            row.append(bar)
            bars.append(bar)
        card.append(title)
        card.append(detail)
        card.append(row)
        window.set_child(card)
        style = Gtk.CssProvider()
        style.load_from_data(
            f"""
            window {{
              background: transparent;
            }}
            .card {{
              background: alpha({colours["background"]}, 0.94);
              color: {colours["text"]};
              border: 1px solid {colours["border"]};
              border-radius: 14px;
              min-width: 420px;
            }}
            label {{
              color: {colours["text"]};
              font-size: 15px;
            }}
            label.title {{
              font-weight: 600;
            }}
            .level {{
              background: {colours["accent"]};
              border-radius: 2px;
              min-width: 5px;
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
