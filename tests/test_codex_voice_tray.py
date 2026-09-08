"""Tray presentation never offers delivery for incomplete or empty audio."""

import asyncio
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
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
        state = {"pane": "p1", "phase": "idle"}
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
        try:
            with patch.dict(os.environ, {"DBUS_SESSION_BUS_ADDRESS": address}):
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
                    await asyncio.wait_for(item.get_title(), 0.5), "Codex Voice"
                )
                action_release.set()
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
                self.assertEqual(values["Title"].value, "Codex Voice")
                self.assertTrue(layout[1][2])
                await asyncio.sleep(0.3)
                self.assertEqual(len(status_calls), calls_when_blocked)
                tray.stop()
                await wait_until(lambda: not tray.thread.is_alive(), 1)
                self.assertFalse(status_release.is_set())
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
            daemon.terminate()
            daemon.wait(timeout=3)
            daemon.stdout.close()


if __name__ == "__main__":
    unittest.main()
