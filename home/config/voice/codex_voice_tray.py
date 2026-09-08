"""Optional COSMIC/StatusNotifier tray, independent of the audio worker."""

import asyncio
import sys
import threading
import time


def presentation(status):
    """Return only public state, never dictated text or assistant replies."""
    phase = status.get("phase", "idle")
    busy = phase in ("starting", "recording", "stopping", "transcribing")
    selected = bool(status.get("pane"))
    ready = bool(status.get("draft") or status.get("pending"))
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
        detail = " ".join(str(status["error"]).split())
        detail = "".join(char for char in detail if char.isprintable())[:120]
        label = f"Voice error: {detail}"
    elif not busy and status.get("pending"):
        label, colour = "Dictation waiting for Codex", "green"
    return {
        "label": label,
        "colour": colour,
        "actions": {
            "record": (
                "Stop and transcribe"
                if phase == "recording"
                else "Start recording",
                selected and (not busy or phase == "recording"),
            ),
            "send": ("Send dictation", selected and not busy and ready),
            "stop": (
                "Cancel / stop speech",
                busy or bool(status.get("pending") or status.get("speaking")),
            ),
            "read": (
                "Read latest reply",
                selected and not busy and bool(status.get("reply")),
            ),
        },
    }


def icon_pixmap(colour, size=32):
    """Supply network-order ARGB so COSMIC preserves recording colours."""
    rgb = {
        "grey": (100, 110, 120),
        "red": (224, 48, 62),
        "amber": (205, 154, 30),
        "green": (42, 164, 95),
        "orange": (226, 104, 30),
    }[colour]
    pixels = bytearray()
    for y in range(size):
        for x in range(size):
            px, py = (x + 0.5) * 32 / size, (y + 0.5) * 32 / size
            circle = (px - 16) ** 2 + (py - 16) ** 2 < 15**2
            capsule = (
                (12 <= px <= 20 and 10 <= py <= 17)
                or (px - 16) ** 2 + (py - 10) ** 2 < 4**2
                or (px - 16) ** 2 + (py - 17) ** 2 < 4**2
            )
            cradle = 36 < (px - 16) ** 2 + (py - 16) ** 2 < 64 and py >= 16
            stand = (15 <= px <= 17 and 22 <= py <= 26) or (
                12 <= px <= 20 and 25 <= py <= 27
            )
            pixels.extend(
                (255, 255, 255, 255)
                if capsule or cradle or stand
                else (255, *rgb)
                if circle
                else (0, 0, 0, 0)
            )
    return [[size, size, bytes(pixels)]]


def _interfaces(tray):
    # Import only in the tray worker. Recording does not depend on D-Bus.
    from dbus_next import Variant
    from dbus_next.constants import PropertyAccess
    from dbus_next.service import (
        ServiceInterface,
        dbus_property,
        method,
        signal,
    )

    def readonly():
        return dbus_property(access=PropertyAccess.READ)

    class Item(ServiceInterface):
        def __init__(self):
            super().__init__("org.kde.StatusNotifierItem")

        @readonly()
        def Category(self) -> "s":
            return "Hardware"

        @readonly()
        def Id(self) -> "s":
            return "codex-voice"

        @readonly()
        def Title(self) -> "s":
            return "Codex Voice"

        @readonly()
        def Status(self) -> "s":
            return "Active"

        @readonly()
        def WindowId(self) -> "u":
            return 0

        @readonly()
        def IconName(self) -> "s":
            return ""

        @readonly()
        def IconPixmap(self) -> "a(iiay)":
            return icon_pixmap(tray.view["colour"])

        @readonly()
        def IconThemePath(self) -> "s":
            return ""

        @readonly()
        def ItemIsMenu(self) -> "b":
            return True

        @readonly()
        def Menu(self) -> "o":
            return "/Menu"

        @readonly()
        def ToolTip(self) -> "(sa(iiay)ss)":
            return ["", [], "Codex Voice", tray.view["label"]]

        @signal()
        def NewIcon(self):
            pass

        @signal()
        def NewToolTip(self):
            pass

        @method()
        def Activate(self, x: "i", y: "i"):
            pass

        @method()
        def ContextMenu(self, x: "i", y: "i"):
            pass

    class Menu(ServiceInterface):
        def __init__(self):
            super().__init__("com.canonical.dbusmenu")
            self.revision = 0

        @readonly()
        def Version(self) -> "u":
            return 3

        @readonly()
        def TextDirection(self) -> "s":
            return "ltr"

        @readonly()
        def Status(self) -> "s":
            return "normal"

        def rows(self):
            rows = [(1, tray.view["label"], False)]
            rows += [
                (i, label, enabled)
                for i, (label, enabled) in enumerate(
                    tray.view["actions"].values(), 2
                )
            ]
            return [
                [
                    i,
                    {
                        "label": Variant("s", label),
                        "enabled": Variant("b", enabled),
                    },
                    [],
                ]
                for i, label, enabled in rows
            ]

        @method()
        def GetLayout(
            self, parent: "i", depth: "i", names: "as"
        ) -> "u(ia{sv}av)":
            rows = self.rows()
            if parent == 0:
                children = (
                    [Variant("(ia{sv}av)", row) for row in rows]
                    if depth != 0
                    else []
                )
                layout = [
                    0,
                    {"children-display": Variant("s", "submenu")},
                    children,
                ]
            else:
                layout = next(row for row in rows if row[0] == parent)
            return [self.revision, layout]

        @method()
        def Event(self, item: "i", event: "s", data: "v", timestamp: "u"):
            actions = list(tray.view["actions"])
            if event == "clicked" and 2 <= item < 2 + len(actions):
                tray.action(actions[item - 2])

        @method()
        def AboutToShow(self, item: "i") -> "b":
            return False

        @signal()
        def LayoutUpdated(self, revision, parent) -> "ui":
            return [revision, parent]

        @signal()
        def ItemsPropertiesUpdated(self, updated, removed) -> "a(ia{sv})a(ias)":
            return [updated, removed]

    return Item(), Menu()


class VoiceTray:
    """Keep tray failures and desktop restarts independent of voice capture."""

    def __init__(self, status_callback, action_callback):
        self.status_callback, self.action_callback = (
            status_callback,
            action_callback,
        )
        self.view = presentation({})
        self.snapshot = self.view
        self.stopped = threading.Event()
        self.action_lock = threading.Lock()
        self.thread = None
        self.sampler = None

    def start(self):
        if self.thread and self.thread.is_alive():
            return
        self.stopped.clear()
        if not self.sampler or not self.sampler.is_alive():
            self.sampler = threading.Thread(
                target=self._sample, name="voice-tray-status", daemon=True
            )
            self.sampler.start()
        self.thread = threading.Thread(
            target=self._run, name="voice-tray", daemon=True
        )
        self.thread.start()

    def stop(self):
        self.stopped.set()

    def _sample(self):
        # Controller status may wait on terminal I/O. Keep only one reader,
        # never join it during shutdown, and publish complete snapshots.
        warned = False
        while not self.stopped.is_set():
            try:
                self.snapshot = presentation(self.status_callback())
                warned = False
            except Exception as exc:
                if not warned:
                    print(
                        f"Voice tray status unavailable: {exc}",
                        file=sys.stderr,
                        flush=True,
                    )
                    warned = True
            self.stopped.wait(0.2)

    def action(self, name):
        locked = name != "stop"
        if self.stopped.is_set():
            return
        if locked and not self.action_lock.acquire(blocking=False):
            return

        def invoke():
            try:
                # Recheck live state because the displayed menu may be stale.
                view = presentation(self.status_callback())
                if view["actions"].get(name, ("", False))[1]:
                    self.action_callback(name)
            except Exception as exc:
                print(
                    f"Voice tray action failed: {exc}",
                    file=sys.stderr,
                    flush=True,
                )
            finally:
                if locked:
                    self.action_lock.release()

        threading.Thread(
            target=invoke, name="voice-tray-action", daemon=True
        ).start()

    def _run(self):
        try:
            asyncio.run(self._serve())
        except Exception as exc:
            print(f"Voice tray unavailable: {exc}", file=sys.stderr, flush=True)
        finally:
            self.stopped.set()

    async def _serve(self):
        from dbus_next import Message, MessageType
        from dbus_next.aio import MessageBus
        from dbus_next.constants import NameFlag, RequestNameReply

        watcher = "org.kde.StatusNotifierWatcher"
        warned = False
        while not self.stopped.is_set():
            bus = None
            try:
                bus = MessageBus()
                await asyncio.wait_for(bus.connect(), 2)
                item, menu = _interfaces(self)
                bus.export("/StatusNotifierItem", item)
                bus.export("/Menu", menu)
                result = await asyncio.wait_for(
                    bus.request_name(
                        "org.willfish.CodexVoice", NameFlag.DO_NOT_QUEUE
                    ),
                    2,
                )
                if result != RequestNameReply.PRIMARY_OWNER:
                    return
                owner, next_check = None, 0
                while bus.connected and not self.stopped.is_set():
                    view = self.snapshot
                    if view != self.view:
                        changed_icon = view["colour"] != self.view["colour"]
                        self.view = view
                        if changed_icon:
                            item.NewIcon()
                        item.NewToolTip()
                        menu.revision += 1
                        menu.LayoutUpdated(menu.revision, 0)
                    if time.monotonic() >= next_check:
                        next_check = time.monotonic() + 2
                        response = await asyncio.wait_for(
                            bus.call(
                                Message(
                                    destination="org.freedesktop.DBus",
                                    path="/org/freedesktop/DBus",
                                    interface="org.freedesktop.DBus",
                                    member="GetNameOwner",
                                    signature="s",
                                    body=[watcher],
                                )
                            ),
                            2,
                        )
                        current = (
                            response.body[0]
                            if response.message_type != MessageType.ERROR
                            else None
                        )
                        if current != owner and current:
                            response = await asyncio.wait_for(
                                bus.call(
                                    Message(
                                        destination=watcher,
                                        path="/StatusNotifierWatcher",
                                        interface=watcher,
                                        member="RegisterStatusNotifierItem",
                                        signature="s",
                                        body=["org.willfish.CodexVoice"],
                                    )
                                ),
                                2,
                            )
                            if response.message_type == MessageType.ERROR:
                                raise RuntimeError(
                                    "Desktop refused the voice tray"
                                )
                            warned = False
                        owner = current
                    await asyncio.sleep(0.2)
            except Exception as exc:
                if not warned and not self.stopped.is_set():
                    print(
                        f"Voice tray reconnecting: {exc}",
                        file=sys.stderr,
                        flush=True,
                    )
                    warned = True
            finally:
                if bus:
                    bus.disconnect()
                    try:
                        await asyncio.wait_for(bus.wait_for_disconnect(), 0.5)
                    except Exception:
                        pass
            for _ in range(10):
                if self.stopped.is_set():
                    break
                await asyncio.sleep(0.2)
