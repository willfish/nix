"""Native voice pill. Rendering never receives transcript text.

A fixed transparent layer surface contains the animated capsule, avoiding
window resize churn. The surface has no input region or keyboard focus.
"""

import math
import time

WIDTH, HEIGHT = 420, 104
BARS = 17


def finite(value, default=0.0):
    try:
        number = float(value)
        return number if math.isfinite(number) else default
    except (ValueError, TypeError, OverflowError):
        return default


def pill_view(view):
    """Project the public OSD view, not the controller's private status."""
    recording = bool(view.get("recording"))
    title = view.get("title", "Voice")
    muted = recording and bool(view.get("muted"))
    clipping = recording and bool(view.get("clipping"))
    warning = muted or clipping
    if muted:
        title = "Microphone muted"
    elif clipping:
        title = "Input too loud"
    compact_status = view.get("phase") in (
        "stopping",
        "transcribing",
    ) or title in (
        "Speaking",
        "About to speak",
    )
    expanded = bool(view.get("visible")) and (
        warning
        or recording
        and finite(view.get("seconds")) < 2
        or not recording
        and not compact_status
    )
    # Normal recording gets a small destination reveal before settling down.
    return {
        "visible": bool(view.get("visible")),
        "recording": recording,
        "level": 0.0
        if muted or not recording
        else max(0.0, min(1.0, finite(view.get("level")))),
        "title": title,
        "detail": view.get("detail", ""),
        "tone": "orange" if warning else view.get("tone", "muted"),
        "timer": view.get("timer", "00:00"),
        "warning": warning,
        "muted": muted,
        "working": view.get("phase")
        in ("starting", "stopping", "transcribing"),
        "expanded": expanded,
        "width": 352.0 if expanded else 208.0,
        "height": 76.0 if expanded else 44.0,
    }


class WarningFilter:
    """Suppress warnings from latched peaks and device-start transients."""

    def __init__(self):
        self.clipping_since = None
        self.last_seconds = None

    def apply(self, view, now):
        view = dict(view)
        seconds = finite(view.get("seconds"))
        if not view.get("recording") or (
            self.last_seconds is not None and seconds < self.last_seconds
        ):
            self.clipping_since = None
        self.last_seconds = seconds if view.get("recording") else None
        if not view.get("recording") or not view.get("clipping"):
            self.clipping_since = None
            return view
        if self.clipping_since is None:
            self.clipping_since = now
        # Capture latches each peak for 1.5s. Outwait that latch so one isolated
        # peak cannot expand the pill, including after microphone startup.
        confirmed = seconds >= 2 and now - self.clipping_since >= 1.6
        if not confirmed:
            view["clipping"] = False
            suffix = ", clipping" if view.get("muted") else " · clipping"
            view["detail"] = view.get("detail", "").removesuffix(suffix)
        return view


class Motion:
    """Frame-rate independent capsule easing and a measured audio history."""

    def __init__(self):
        self.width, self.height, self.opacity = 176.0, 36.0, 0.0
        self.levels = [0.0] * BARS
        self.last = None
        self.sample_at = None

    def step(self, view, now, reduced=False):
        dt = (
            min(0.1, max(0.0, now - self.last))
            if self.last is not None
            else 1 / 60
        )
        self.last = now
        visible = view["visible"]
        target_w = view["width"] if visible else 176.0
        target_h = view["height"] if visible else 36.0
        ease = 1.0 if reduced else 1 - math.exp(-dt / 0.085)
        self.width += (target_w - self.width) * ease
        self.height += (target_h - self.height) * ease
        self.opacity += ((1.0 if visible else 0.0) - self.opacity) * ease
        if not visible or not view["recording"] or view["muted"]:
            self.levels = [0.0] * BARS
            self.sample_at = None
        elif self.sample_at is None or now - self.sample_at >= 0.065:
            # Every bar is measured history, never random decorative activity.
            level = view["level"]
            scaled = max(
                0.0, min(1.0, (20 * math.log10(max(1e-6, level)) + 50) / 40)
            )
            self.levels = self.levels[1:] + [scaled]
            self.sample_at = now
        return visible or self.opacity > 0.01


def rounded(ctx, x, y, width, height, radius):
    radius = min(radius, width / 2, height / 2)
    ctx.new_sub_path()
    for cx, cy, start in (
        (x + width - radius, y + radius, -math.pi / 2),
        (x + width - radius, y + height - radius, 0),
        (x + radius, y + height - radius, math.pi / 2),
        (x + radius, y + radius, math.pi),
    ):
        ctx.arc(cx, cy, radius, start, start + math.pi / 2)
    ctx.close_path()


def rgb(value):
    value = value.lstrip("#")
    if len(value) == 3:
        value = "".join(char * 2 for char in value)
    try:
        return tuple(int(value[i : i + 2], 16) / 255 for i in (0, 2, 4))
    except (ValueError, IndexError):
        return (0.7, 0.7, 0.7)


def paint(ctx, view, motion, colours, now, reduced=False):
    """Draw with Cairo/Pango both in GTK and in offline synthetic previews."""
    import gi

    gi.require_version("Pango", "1.0")
    gi.require_version("PangoCairo", "1.0")
    from gi.repository import Pango, PangoCairo

    w, h = motion.width, motion.height
    x, y = (WIDTH - w) / 2, 12 + (1 - motion.opacity) * -6
    tone = rgb(colours.get(view["tone"], "#9c9ca5"))
    ctx.save()
    ctx.push_group()
    for spread, alpha in ((7, 0.025), (4, 0.04), (2, 0.07)):
        rounded(
            ctx,
            x - spread,
            y + 3 - spread,
            w + spread * 2,
            h + spread * 2,
            h / 2 + spread,
        )
        ctx.set_source_rgba(0, 0, 0, alpha)
        ctx.fill()
    rounded(ctx, x, y, w, h, min(26, h / 2))
    ctx.set_source_rgba(0.047, 0.051, 0.063, 0.97)
    ctx.fill_preserve()
    ctx.set_source_rgba(1, 1, 1, 0.13)
    ctx.set_line_width(1)
    ctx.stroke()
    ctx.save()
    rounded(ctx, x + 1, y + 1, w - 2, h - 2, min(25, h / 2))
    ctx.clip()

    def text(
        value,
        tx,
        ty,
        max_width,
        size=11,
        bold=False,
        colour=(0.95, 0.95, 0.97),
    ):
        layout = PangoCairo.create_layout(ctx)
        layout.set_font_description(
            Pango.FontDescription(f"Sans {'Bold ' if bold else ''}{size}")
        )
        layout.set_text(str(value), -1)
        layout.set_width(max(1, int(max_width)) * Pango.SCALE)
        layout.set_ellipsize(Pango.EllipsizeMode.END)
        layout.set_single_paragraph_mode(True)
        ctx.move_to(tx, ty)
        ctx.set_source_rgb(*colour)
        PangoCairo.show_layout(ctx, layout)

    cy = y + 22
    ctx.set_source_rgb(*tone)
    if view["working"]:
        ctx.set_line_width(2)
        angle = 0 if reduced else now * 4
        ctx.arc(x + 22, cy, 5, angle, angle + math.pi * 1.45)
        ctx.stroke()
    elif (
        view["warning"]
        or view["tone"] in ("orange", "red")
        and not view["recording"]
    ):
        text("!", x + 18, cy - 10, 12, 12, True, tone)
    elif view["tone"] == "green":
        ctx.set_line_width(2)
        ctx.move_to(x + 17, cy)
        ctx.line_to(x + 21, cy + 4)
        ctx.line_to(x + 28, cy - 4)
        ctx.stroke()
    else:
        # A steady, unmistakable capture light. No pulsing decorative hot mic.
        ctx.arc(x + 22, cy, 4 if view["recording"] else 3, 0, math.tau)
        ctx.fill()
    if view["recording"] and not view["warning"]:
        bar_x = x + 43
        for index, level in enumerate(motion.levels):
            bar_h = 2 + level * 17
            rounded(
                ctx, bar_x + index * 4.5, cy - bar_h / 2, 2.5, bar_h, 1.25
            )
            ctx.set_source_rgba(0.95, 0.95, 0.97, 0.4 + 0.6 * level)
            ctx.fill()
        text(view["timer"], x + w - 63, cy - 9, 53, 10, True)
    else:
        text(view["title"], x + 39, cy - 10, w - 58, 11, True)
    if view["expanded"]:
        text(
            view["detail"],
            x + 22,
            y + 45,
            w - 44,
            9,
            colour=(0.66, 0.68, 0.73),
        )
    ctx.restore()
    ctx.pop_group_to_source()
    ctx.paint_with_alpha(max(0, min(1, motion.opacity)))
    ctx.restore()


def run_pill(
    osd_view, read_status, read_notice, read_json, focused_connector, colours
):
    import threading
    import gi

    gi.require_version("Gdk", "4.0")
    gi.require_version("Gtk", "4.0")
    gi.require_version("Gtk4LayerShell", "1.0")
    gi.require_foreign("cairo")
    import cairo
    from gi.repository import Gdk, GLib, Gtk, Gtk4LayerShell

    app = Gtk.Application(application_id="uk.hues.pi-voice-osd")
    stop = threading.Event()
    lock = threading.Lock()
    shared = {
        "view": pill_view(osd_view({})),
        "connector": None,
        "sampled": time.monotonic(),
    }
    motion = Motion()
    state = {"view": shared["view"], "animating": False, "connector": None}

    def unavailable():
        return pill_view(
            osd_view(
                {
                    "phase": "error",
                    "error": "Voice connection lost",
                    "session_label": "Microphone state unknown",
                }
            )
        )

    def poll():
        next_monitor = 0
        was_recording = False
        warnings = WarningFilter()
        while not stop.is_set():
            # Socket and hyprctl waits never block the GTK render/event thread.
            status = read_status()
            if status is None and was_recording:
                warnings.apply(osd_view({}), time.monotonic())
                with lock:
                    shared["view"] = unavailable()
                    shared["sampled"] = time.monotonic()
                stop.wait(0.08)
                continue
            view = osd_view(status or {})
            was_recording = view["recording"]
            if not view["visible"]:
                view = read_notice() or view
            with lock:
                now = time.monotonic()
                shared["view"] = pill_view(warnings.apply(view, now))
                shared["sampled"] = now
            if time.monotonic() >= next_monitor:
                connector = focused_connector(
                    read_json(["hyprctl", "monitors", "-j"])
                )
                with lock:
                    shared["connector"] = connector
                next_monitor = time.monotonic() + 1
            stop.wait(0.08)

    def activate(_app):
        window = Gtk.ApplicationWindow(application=app)
        window.set_name("voice-pill")
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
        canvas = Gtk.DrawingArea()
        canvas.set_content_width(WIDTH)
        canvas.set_content_height(HEIGHT)
        canvas.set_can_target(False)
        window.set_child(canvas)
        css = Gtk.CssProvider()
        css.load_from_data(
            b"window#voice-pill { background: transparent; box-shadow: none; }",
            -1,
        )
        # Structural transparency must beat gtk.css's global background.
        # Scope the override to this surface, not the user's theme in general.
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(),
            css,
            Gtk.STYLE_PROVIDER_PRIORITY_USER + 1,
        )
        settings = Gtk.Settings.get_default()

        def reduced():
            return not settings.get_property("gtk-enable-animations")

        def draw(_area, ctx, _width, _height):
            paint(
                ctx,
                state["view"],
                motion,
                colours,
                time.monotonic(),
                reduced(),
            )

        canvas.set_draw_func(draw)

        def frame(_widget, _clock):
            keep = motion.step(state["view"], time.monotonic(), reduced())
            canvas.queue_draw()
            if not keep:
                window.set_visible(False)
            view = state["view"]
            settling = (
                abs(motion.width - view["width"]) > 0.1
                or abs(motion.height - view["height"]) > 0.1
                or abs(motion.opacity - float(view["visible"])) > 0.005
            )
            changing = (
                view["recording"]
                and not view["muted"]
                or view["working"]
                and not reduced()
            )
            state["animating"] = keep and (
                settling or changing or not view["visible"]
            )
            return state["animating"]

        def update():
            with lock:
                view, connector, sampled = (
                    shared["view"],
                    shared["connector"],
                    shared["sampled"],
                )
            # A stale capture status is unknown, not evidence the mic is closed.
            if time.monotonic() - sampled > 2:
                view = (
                    unavailable()
                    if view["recording"]
                    else pill_view(osd_view({}))
                )
            changed = view != state["view"]
            state["view"] = view
            label = (
                f"{view['title']}. {view['detail']}"
                if view["visible"]
                else "Voice idle"
            )
            if label != state.get("accessible_label"):
                canvas.update_property(
                    [Gtk.AccessibleProperty.LABEL], [label]
                )
                state["accessible_label"] = label
            if connector and connector != state["connector"]:
                monitors = Gdk.Display.get_default().get_monitors()
                for index in range(monitors.get_n_items()):
                    monitor = monitors.get_item(index)
                    if monitor.get_connector() == connector:
                        Gtk4LayerShell.set_monitor(window, monitor)
                        state["connector"] = connector
                        break
            if view["visible"] and not window.get_visible():
                window.set_visible(True)
                surface = window.get_surface()
                if surface:
                    surface.set_input_region(cairo.Region())
            if (
                changed
                and not state["animating"]
                and (view["visible"] or window.get_visible())
            ):
                state["animating"] = True
                canvas.add_tick_callback(frame)
            return True

        GLib.timeout_add(80, update)
        threading.Thread(
            target=poll, name="voice-pill-status", daemon=True
        ).start()

    app.connect("activate", activate)
    app.connect("shutdown", lambda *_: stop.set())
    try:
        return app.run(["pi-voice-osd"])
    finally:
        stop.set()
