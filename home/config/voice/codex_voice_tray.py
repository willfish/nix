"""Optional COSMIC/StatusNotifier tray, independent of the audio worker."""

import asyncio
import sys
import threading
import time


def public_label(value, limit=120):
    text = " ".join(str(value).split())
    return "".join(char for char in text if char.isprintable())[:limit]


def presentation(status):
    """Return only public state, never dictated text or assistant replies."""
    phase = status.get("phase", "idle")
    busy = phase in ("starting", "recording", "stopping", "transcribing")
    selected = bool(status.get("pane"))
    ready = bool(status.get("draft") or status.get("pending"))
    pending = bool(status.get("pending"))
    retry = bool(status.get("retry"))
    speaking = bool(status.get("speaking"))
    responding = bool(status.get("responding")) or (
        status.get("agent_state") == "working"
    )
    blocked = status.get("agent_state") == "blocked"
    harness = public_label(status.get("harness") or "Codex", 30)
    harness = {"codex": "Codex", "grok": "Grok", "pi": "Pi",
               "qwen-pi": "Qwen Pi"}.get(harness, harness)
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
    context = []
    if selected:
        session = public_label(status.get("session_label") or status["pane"])
        context.append(f"{harness}: {session}")
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
        "selected_session": None,
        "actions": {"auto-toggle": ("Read replies aloud", True)},
    }
    actions = view["actions"]
    for character, label in status.get("voices", {}).items():
        actions["voice:" + character] = (public_label(label), True)
    if selected and not busy and not pending and not speaking \
            and not responding and status.get("reply"):
        actions["read"] = ("Replay last reply", True)
    if busy or speaking:
        actions["stop"] = (
            "Cancel transcription" if phase == "transcribing"
            else "Cancel recording" if busy else "Stop speaking", True,
        )
    if selected and ready and not busy:
        actions["append"] = ("Record more", True)
    if selected and retry and not busy:
        actions["retry"] = ("Retry transcription", True)
    if (pending or retry) and not busy:
        actions["discard"] = (
            "Discard retained dictation" if pending
            else "Discard retained recording", True,
        )
    if selected and status.get("rebind_needed") and not busy:
        actions["rebind"] = ("Bind to current conversation", True)
    for session in status.get("sessions", []):
        token = session.get("token")
        if not isinstance(token, str) or not token:
            continue
        selected_session = bool(session.get("selected"))
        if selected_session:
            view["selected_session"] = f"select:{token}"
        view["actions"][f"select:{token}"] = (
            public_label(session.get("label") or token),
            not busy and not selected_session,
        )
    return view


def icon_pixmap(colour, size=32, glyph="microphone"):
    """Supply network-order ARGB so COSMIC preserves recording colours."""
    rgb = {
        "grey": (100, 110, 120),
        "red": (224, 48, 62),
        "amber": (205, 154, 30),
        "green": (42, 164, 95),
        "orange": (226, 104, 30),
        "blue": (48, 125, 224),
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
            mark = capsule or cradle or stand
            if glyph == "dots":
                mark = any((px - cx) ** 2 + (py - 16) ** 2 < 2.3**2
                           for cx in (8, 16, 24))
            elif glyph == "blocked":
                mark = (14.5 <= px <= 17.5 and 7 <= py <= 19) or (
                    (px - 16) ** 2 + (py - 24) ** 2 < 2**2
                )
            elif glyph == "speaker":
                mark = (7 <= px <= 12 and 12 <= py <= 20) or (
                    12 <= px <= 18 and abs(py - 16) <= px - 8
                ) or (px >= 21 and 49 < (px - 16) ** 2 + (py - 16) ** 2 < 81)
            elif glyph == "check":
                mark = (8 <= px <= 14 and abs(py - px - 7) < 1.5) or (
                    14 <= px <= 25 and abs(py + px - 35) < 1.5
                )
            pixels.extend(
                (255, 255, 255, 255)
                if mark
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
            return "Agent Voice"

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
            return icon_pixmap(tray.view["colour"], glyph=tray.view["glyph"])

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
            detail = "\n".join([tray.view["label"], *tray.view["context"]])
            return ["", [], "Agent Voice", detail]

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
            rows = [
                (tray.action_id(action), label, enabled)
                for action, (label, enabled) in tray.view["actions"].items()
                if not action.startswith(("select:", "voice:"))
            ]
            rows = [
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
            for row in rows:
                if tray.actions_by_id[row[0]] == "auto-toggle":
                    row[1].update({
                        "toggle-type": Variant("s", "checkmark"),
                        "toggle-state": Variant("i", int(tray.view["auto"])),
                    })
            sessions = [
                Variant("(ia{sv}av)", [
                    tray.action_id(action),
                    {
                        "label": Variant("s", label),
                        "enabled": Variant("b", enabled),
                        "toggle-type": Variant("s", "radio"),
                        "toggle-state": Variant(
                            "i", int(action == tray.view["selected_session"])
                        ),
                    },
                    [],
                ])
                for action, (label, enabled) in tray.view["actions"].items()
                if action.startswith("select:")
            ]
            # This ID is deliberately outside actions_by_id: opening the
            # submenu must not dispatch a voice command.
            selector = [11, {
                "label": Variant("s", "Voice session"),
                "enabled": Variant("b", bool(sessions)),
                "children-display": Variant("s", "submenu"),
            }, sessions]
            voices = [
                Variant(
                    "(ia{sv}av)",
                    [
                        tray.action_id(action),
                        {
                            "label": Variant("s", label),
                            "enabled": Variant("b", enabled),
                            "toggle-type": Variant("s", "radio"),
                            "toggle-state": Variant(
                                "i",
                                int(
                                    action
                                    == "voice:" + tray.view["selected_voice"]
                                ),
                            ),
                        },
                        [],
                    ],
                )
                for action, (label, enabled) in tray.view["actions"].items()
                if action.startswith("voice:")
            ]
            voice_selector = [13, {
                "label": Variant("s", "Character voice"),
                "enabled": Variant("b", True),
                "children-display": Variant("s", "submenu"),
            }, voices]
            return [selector, *([voice_selector] if voices else []), *rows]

        @method()
        def GetLayout(
            self, parent: "i", depth: "i", names: "as"
        ) -> "u(ia{sv}av)":
            root = [
                0, {"children-display": Variant("s", "submenu")},
                [Variant("(ia{sv}av)", row) for row in self.rows()],
            ]

            def find(row):
                if row[0] == parent:
                    return row
                for child in row[2]:
                    found = find(child.value)
                    if found is not None:
                        return found
                return None

            def limited(row, remaining):
                return [row[0], row[1], [
                    Variant("(ia{sv}av)", limited(child.value, remaining - 1))
                    for child in row[2]
                ] if remaining != 0 else []]

            layout = find(root)
            if layout is None:
                raise ValueError("Unknown voice menu item")
            return [self.revision, limited(layout, depth)]

        @method()
        def Event(self, item: "i", event: "s", data: "v", timestamp: "u"):
            action = tray.actions_by_id.get(item)
            if event == "clicked" and action:
                tray.action(action)

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
        self.ids_by_action = {
            action: i for i, action in enumerate((
                "record", "send", "stop", "read", "rebind", "retry",
                "discard", "append", "replace",
            ), 2)
        }
        self.ids_by_action["auto-toggle"] = 12
        self.actions_by_id = {
            i: action for action, i in self.ids_by_action.items()
        }
        self.next_action_id = 100

    def action_id(self, action):
        if action not in self.ids_by_action:
            self.ids_by_action[action] = self.next_action_id
            self.actions_by_id[self.next_action_id] = action
            self.next_action_id += 1
        return self.ids_by_action[action]

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
                if name == "stop":
                    self.action_callback(name)
                    return
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
                        changed_icon = any(view[key] != self.view[key]
                                           for key in ("colour", "glyph"))
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
                    # Pinned dbus-next shuts down but leaves both owners open.
                    # Close after its reader/writer callbacks are finalized.
                    bus._stream.close()
                    bus._sock.close()
            for _ in range(10):
                if self.stopped.is_set():
                    break
                await asyncio.sleep(0.2)
