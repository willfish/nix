"""Tray presentation never offers delivery for incomplete or empty audio."""

import asyncio
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

MODULE = (
    Path(__file__).resolve().parents[1]
    / "home/config/voice/codex_voice_tray.py"
)


class PresentationTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "Voice tray is not implemented")
        spec = importlib.util.spec_from_file_location(
            "codex_voice_tray", MODULE
        )
        self.tray = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tray)

    def test_starting_does_not_claim_recording_or_allow_send(self):
        view = self.tray.presentation({"pane": "p1", "phase": "starting"})
        self.assertEqual(view["label"], "Starting microphone")
        self.assertEqual(view["colour"], "amber")
        self.assertFalse(view["actions"]["send"][1])
        self.assertTrue(view["actions"]["stop"][1])

    def test_recording_displays_elapsed_time_and_only_stop_capture_actions(
        self,
    ):
        view = self.tray.presentation(
            {
                "pane": "p1",
                "phase": "recording",
                "recording": True,
                "recording_seconds": 65.9,
                "input_level": 0.8,
                "draft": True,
            }
        )
        self.assertIn("01:05", view["label"])
        self.assertEqual(view["colour"], "red")
        self.assertEqual(
            view["actions"]["record"], ("Stop and transcribe", True)
        )
        self.assertFalse(view["actions"]["send"][1])
        self.assertFalse(view["actions"]["read"][1])

    def test_empty_idle_and_busy_states_cannot_submit(self):
        for phase in ("idle", "starting", "stopping", "transcribing", "error"):
            with self.subTest(phase=phase):
                view = self.tray.presentation({"pane": "p1", "phase": phase})
                self.assertFalse(view["actions"]["send"][1])

    def test_pending_dictation_can_be_sent_only_with_a_selected_session(self):
        for pane in (None, "p1"):
            view = self.tray.presentation(
                {"pane": pane, "phase": "draft", "pending": True}
            )
            self.assertEqual(view["actions"]["send"][1], bool(pane))

    def test_cancel_is_not_offered_for_text_already_staged_in_terminal(self):
        view = self.tray.presentation(
            {"pane": "p1", "phase": "draft", "draft": True}
        )
        self.assertTrue(view["actions"]["send"][1])
        self.assertFalse(view["actions"]["stop"][1])

    def test_presentation_does_not_include_transcript_or_reply(self):
        view = self.tray.presentation(
            {
                "pane": "p1",
                "phase": "error",
                "error": "Microphone failed",
                "reply": "private answer",
                "transcript": "private request",
            }
        )
        self.assertNotIn("private", str(view))

    def test_error_detail_is_visible_bounded_and_without_control_characters(
        self,
    ):
        view = self.tray.presentation(
            {
                "phase": "error",
                "error": "Microphone disconnected\n\x1b" + "x" * 200,
            }
        )
        self.assertIn("Microphone disconnected", view["label"])
        self.assertNotIn("\n", view["label"])
        self.assertNotIn("\x1b", view["label"])
        self.assertLessEqual(len(view["label"]), 150)

    def test_cancel_can_interrupt_a_slow_tray_action(self):
        state = {"pane": "p1", "phase": "idle"}
        began, release, cancelled = (threading.Event() for _ in range(3))

        def action(name):
            if name == "record":
                state["phase"] = "starting"
                began.set()
                release.wait(2)
            if name == "stop":
                cancelled.set()

        tray = self.tray.VoiceTray(lambda: dict(state), action)
        try:
            tray.action("record")
            self.assertTrue(began.wait(1))
            tray.action("stop")
            self.assertTrue(cancelled.wait(0.5))
        finally:
            release.set()
            tray.stop()

    def test_cancel_does_not_wait_for_status_or_check_stale_state(self):
        status_called, cancelled = threading.Event(), threading.Event()

        def status():
            status_called.set()
            return {"phase": "idle"}

        tray = self.tray.VoiceTray(status, lambda _: cancelled.set())
        tray.action("stop")
        self.assertTrue(cancelled.wait(0.5))
        self.assertFalse(status_called.is_set())
        tray.stop()

    def test_context_shows_harness_microphone_and_model_loading(self):
        view = self.tray.presentation({
            "pane": "p1", "harness": "Grok", "session_label": "dotfiles",
            "models": "loading", "microphone": {
                "name": "USB microphone", "muted": True,
                "preferred": "headset", "missing": True,
            },
        })
        context = " ".join(view["context"])
        self.assertIn("Grok: dotfiles", context)
        self.assertIn("USB microphone", context)
        self.assertIn("muted", context)
        self.assertIn("preferred microphone unavailable", context)
        self.assertIn("Speech models loading", context)
        self.assertEqual(view["colour"], "amber")

    def test_retained_text_offers_append_replace_discard_and_retry(self):
        view = self.tray.presentation({
            "pane": "p1", "phase": "draft", "pending": True,
            "harness": "Pi", "retry": True,
        })
        self.assertIn("Pi", view["label"])
        for name in ("append", "replace", "discard", "retry"):
            self.assertTrue(view["actions"][name][1])
        fresh = self.tray.presentation({"pane": "p1"})
        self.assertNotIn("append", fresh["actions"])
        self.assertNotIn("replace", fresh["actions"])
        self.assertFalse(fresh["actions"]["retry"][1])

    def test_retained_dictation_disables_read_until_sent_or_discarded(self):
        view = self.tray.presentation({
            "pane": "p1", "phase": "draft", "pending": True,
            "reply": "Latest reply",
        })
        self.assertFalse(view["actions"]["read"][1])

    def test_retry_audio_can_be_cancelled_or_discarded_without_pending_text(
        self,
    ):
        view = self.tray.presentation({
            "pane": "p1", "phase": "error", "retry": True,
        })
        self.assertTrue(view["actions"]["stop"][1])
        self.assertTrue(view["actions"]["discard"][1])
        self.assertIn("recording", view["actions"]["discard"][0])

    def test_changed_conversation_displays_explicit_rebind_guidance(self):
        view = self.tray.presentation({
            "pane": "p1", "harness": "Pi", "session_label": "old",
            "rebind_needed": True,
        })
        self.assertIn("Bind to current conversation", " ".join(
            view["context"]
        ))
        self.assertEqual(view["colour"], "amber")

    def test_sessions_are_individually_selectable_and_rebind_is_available(self):
        view = self.tray.presentation({
            "pane": "p1", "sessions": [
                {"token": "one", "label": "Codex: dotfiles",
                 "selected": True},
                {"token": "two", "label": "Pi: notes", "selected": False},
            ],
        })
        self.assertTrue(view["actions"]["rebind"][1])
        self.assertFalse(view["actions"]["select:one"][1])
        self.assertTrue(view["actions"]["select:two"][1])
        self.assertEqual(view["actions"]["select:one"][0], "Codex: dotfiles")
        self.assertEqual(view["actions"]["select:two"][0], "Pi: notes")
        self.assertEqual(view["selected_session"], "select:one")

    def test_recording_keeps_red_icon_and_reports_clipping(self):
        view = self.tray.presentation({
            "pane": "p1", "phase": "recording", "models": "loading",
            "microphone": {"name": "Mic", "clipping": True},
        })
        self.assertEqual(view["colour"], "red")
        self.assertIn("clipping", " ".join(view["context"]))

    def test_cancel_reaches_real_controller_during_slow_terminal_paste(self):
        source = MODULE.with_name("codex_voice.py")
        spec = importlib.util.spec_from_file_location("codex_voice", source)
        voice = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(voice)
        entered, release, stopped = (threading.Event() for _ in range(3))

        def insert(target, text):
            entered.set()
            release.wait(2)

        terminal = SimpleNamespace(insert=insert)
        audio = SimpleNamespace(stop=stopped.set)
        with tempfile.TemporaryDirectory() as directory:
            controller = voice.Controller(
                Path(directory), terminal, audio, lambda *args: None
            )
            controller.register("one", {"pane": "pane-one"})
            controller.phase = "transcribing"
            stopped.clear()
            tray = self.tray.VoiceTray(
                controller.status, lambda _: controller.stop()
            )
            worker = threading.Thread(
                target=controller.stage, args=("Pending words", "one"),
                daemon=True,
            )
            try:
                worker.start()
                self.assertTrue(entered.wait(1))
                tray.action("stop")
                self.assertTrue(stopped.wait(0.2))
                self.assertTrue(controller.input_cancelled.is_set())
            finally:
                release.set()
                worker.join(1)
                tray.stop()
            self.assertFalse(controller.status()["draft"])
            self.assertFalse(controller.status()["pending"])


@unittest.skipUnless(
    importlib.util.find_spec("dbus_next") and shutil.which("dbus-daemon"),
    "private D-Bus integration needs dbus-next and dbus-daemon",
)
class BusTests(unittest.IsolatedAsyncioTestCase):
    async def test_tray_registers_updates_and_recovers_from_watcher_restart(
        self,
    ):
        from dbus_next import Variant
        from dbus_next.aio import MessageBus
        from dbus_next.service import ServiceInterface, method

        self.assertTrue(MODULE.exists(), "Voice tray is not implemented")
        spec = importlib.util.spec_from_file_location(
            "codex_voice_tray", MODULE
        )
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertTrue(
            hasattr(module, "VoiceTray"), "Tray service is not implemented"
        )
        daemon = subprocess.Popen(
            ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        address = daemon.stdout.readline().strip()
        state = {
            "pane": "p1", "phase": "idle", "harness": "Pi",
            "session_label": "dotfiles", "models": "loading",
            "microphone": {"name": "USB mic"},
        }
        actions = []
        action_release = threading.Event()
        block_status = threading.Event()
        status_blocked = threading.Event()
        status_release = threading.Event()
        status_calls = []

        def status():
            status_calls.append(True)
            if block_status.is_set():
                status_blocked.set()
                status_release.wait(5)
            return dict(state)

        def action(name):
            actions.append(name)
            if name.startswith("select:"):
                for session in state.get("sessions", []):
                    session["selected"] = name == "select:" + session["token"]
            action_release.wait(3)

        class Watcher(ServiceInterface):
            def __init__(self):
                super().__init__("org.kde.StatusNotifierWatcher")
                self.items = []

            @method()
            def RegisterStatusNotifierItem(self, service: "s"):
                self.items.append(service)

        async def wait_until(condition, seconds=5):
            deadline = time.monotonic() + seconds
            while not condition() and time.monotonic() < deadline:
                await asyncio.sleep(0.02)
            self.assertTrue(condition(), "Timed out waiting for tray update")

        watcher_bus = await MessageBus(bus_address=address).connect()
        client = await MessageBus(bus_address=address).connect()
        watcher = Watcher()
        watcher_bus.export("/StatusNotifierWatcher", watcher)
        tray = module.VoiceTray(status, action)
        tray_buses = []

        class TrackedBus(MessageBus):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)
                tray_buses.append(self)

        try:
            with (
                patch.dict(os.environ, {"DBUS_SESSION_BUS_ADDRESS": address}),
                patch("dbus_next.aio.MessageBus", TrackedBus),
            ):
                tray.start()
                # The desktop watcher can appear after the voice service starts.
                await asyncio.sleep(0.3)
                self.assertTrue(tray.thread.is_alive())
                await watcher_bus.request_name("org.kde.StatusNotifierWatcher")
                await wait_until(lambda: watcher.items)
                dest = watcher.items[0]
                tree = await client.introspect(dest, "/StatusNotifierItem")
                item_object = client.get_proxy_object(
                    dest, "/StatusNotifierItem", tree
                )
                item = item_object.get_interface("org.kde.StatusNotifierItem")
                properties = item_object.get_interface(
                    "org.freedesktop.DBus.Properties"
                )
                tree = await client.introspect(dest, "/Menu")
                menu = client.get_proxy_object(
                    dest, "/Menu", tree
                ).get_interface("com.canonical.dbusmenu")
                icons = []
                item.on_new_icon(lambda: icons.append(True))
                old_pixmap = await item.get_icon_pixmap()
                self.assertEqual(await item.get_icon_name(), "")
                self.assertTrue(await item.get_item_is_menu())
                state.update(
                    phase="recording", recording=True, recording_seconds=12
                )
                await wait_until(lambda: icons)
                self.assertNotEqual(await item.get_icon_pixmap(), old_pixmap)
                _, layout = await menu.call_get_layout(0, -1, [])
                rows = [child.value for child in layout[2]]
                self.assertIn("00:12", rows[0][1]["label"].value)
                labels = [row[1]["label"].value for row in rows]
                self.assertIn("Pi: dotfiles", labels)
                self.assertIn("Microphone: USB mic", labels)
                self.assertIn("Speech models loading", labels)
                send = next(
                    row
                    for row in rows
                    if row[1]["label"].value == "Send dictation"
                )
                await menu.call_event(send[0], "clicked", Variant("i", 0), 0)
                await asyncio.sleep(0.1)
                self.assertEqual(actions, [])
                cancel = next(
                    row
                    for row in rows
                    if row[1]["label"].value == "Cancel / stop speech"
                )
                await menu.call_event(cancel[0], "clicked", Variant("i", 0), 0)
                await wait_until(lambda: actions == ["stop"])
                # A slow controller action must not stall tray D-Bus replies.
                self.assertEqual(
                    await asyncio.wait_for(item.get_title(), 0.5), "Agent Voice"
                )
                action_release.set()
                state.update(phase="idle", sessions=[
                    {"token": "first", "label": "pi: dotfiles",
                     "selected": True},
                    {"token": "second", "label": "grok: notes"},
                ])
                await wait_until(
                    lambda: "select:second" in tray.view["actions"]
                )
                _, layout = await menu.call_get_layout(0, -1, [])
                rows = [child.value for child in layout[2]]
                selector = next(
                    row for row in rows
                    if row[1]["label"].value == "Voice session"
                )
                self.assertEqual(
                    selector[1]["children-display"].value, "submenu"
                )
                self.assertTrue(selector[1]["enabled"].value)
                sessions = [child.value for child in selector[2]]
                self.assertEqual(
                    [row[1]["label"].value for row in sessions],
                    ["pi: dotfiles", "grok: notes"],
                )
                self.assertEqual(
                    [row[1]["toggle-state"].value for row in sessions], [1, 0]
                )
                self.assertTrue(all(
                    row[1]["toggle-type"].value == "radio" for row in sessions
                ))
                _, shallow = await menu.call_get_layout(0, 1, [])
                shallow_selector = next(
                    child.value for child in shallow[2]
                    if child.value[0] == selector[0]
                )
                self.assertEqual(shallow_selector[2], [])
                _, subtree = await menu.call_get_layout(selector[0], -1, [])
                self.assertEqual(subtree[2], selector[2])
                # Opening the submenu must not select anything.
                await menu.call_event(
                    selector[0], "clicked", Variant("i", 0), 0
                )
                old_session = next(
                    row for row in sessions
                    if row[1]["label"].value == "grok: notes"
                )
                await menu.call_event(
                    old_session[0], "clicked", Variant("i", 0), 0
                )
                await wait_until(
                    lambda: tray.view["selected_session"] == "select:second"
                )
                self.assertEqual(actions, ["stop", "select:second"])
                _, subtree = await menu.call_get_layout(selector[0], -1, [])
                self.assertEqual(
                    [child.value[1]["toggle-state"].value
                     for child in subtree[2]], [0, 1]
                )
                _, selected_row = await menu.call_get_layout(
                    old_session[0], 0, []
                )
                self.assertFalse(selected_row[1]["enabled"].value)
                state["sessions"] = [
                    {"token": "third", "label": "codex: third"},
                ]
                await wait_until(
                    lambda: "select:third" in tray.view["actions"]
                )
                # A click from an old menu must never select a different pane.
                await menu.call_event(
                    old_session[0], "clicked", Variant("i", 0), 0
                )
                await asyncio.sleep(0.1)
                self.assertEqual(actions, ["stop", "select:second"])
                await watcher_bus.release_name("org.kde.StatusNotifierWatcher")
                replacement = Watcher()
                client.export("/StatusNotifierWatcher", replacement)
                await client.request_name("org.kde.StatusNotifierWatcher")
                await wait_until(lambda: replacement.items)
                # Herdr operations can hold the controller's status lock.
                block_status.set()
                await wait_until(status_blocked.is_set)
                calls_when_blocked = len(status_calls)
                values, layout = await asyncio.wait_for(
                    asyncio.gather(
                        properties.call_get_all("org.kde.StatusNotifierItem"),
                        menu.call_get_layout(0, -1, []),
                    ),
                    0.5,
                )
                self.assertEqual(values["Title"].value, "Agent Voice")
                self.assertTrue(layout[1][2])
                await asyncio.sleep(0.3)
                self.assertEqual(len(status_calls), calls_when_blocked)
                tray.stop()
                await wait_until(lambda: not tray.thread.is_alive(), 1)
                self.assertFalse(status_release.is_set())
                self.assertTrue(tray_buses)
                for connection in tray_buses:
                    self.assertEqual(connection._sock.fileno(), -1)
                    self.assertTrue(connection._stream.closed)
        finally:
            action_release.set()
            status_release.set()
            tray.stop()
            await asyncio.sleep(0.3)
            watcher_bus.disconnect()
            client.disconnect()
            await asyncio.gather(
                watcher_bus.wait_for_disconnect(), client.wait_for_disconnect()
            )
            # dbus-next disconnect shuts down but does not close these owners.
            for connection in (watcher_bus, client, *tray_buses):
                connection._stream.close()
                connection._sock.close()
            daemon.terminate()
            daemon.wait(timeout=3)
            daemon.stdout.close()


if __name__ == "__main__":
    unittest.main()
