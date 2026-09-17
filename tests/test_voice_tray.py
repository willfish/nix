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
    / "home/config/voice/voice_tray.py"
)


class PresentationTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "Voice tray is not implemented")
        spec = importlib.util.spec_from_file_location(
            "voice_tray", MODULE
        )
        self.tray = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tray)

    def test_team_filter_keeps_selected_child_and_full_labels(self):
        rows = [
            {"token": "a", "id": "1", "team_child": True, "selected": True},
            {"token": "b", "id": "2", "team_child": True},
            {
                "token": "c",
                "id": "3",
                "label": "short",
                "full_label": "x" * 200,
            },
        ]
        view = self.tray.presentation({"sessions": rows})
        self.assertFalse(view["show_team"])
        self.assertIn("select:a", view["actions"])
        self.assertNotIn("select:b", view["actions"])
        self.assertEqual(view["full_labels"]["select:c"], "x" * 200)
        shown = self.tray.presentation({"sessions": rows, "show_team": True})
        self.assertIn("select:b", shown["actions"])
        self.assertEqual(shown["selected_session"], view["selected_session"])

    def test_recovery_is_explicit_and_stage_requires_ready_pi(self):
        base = {"retained": True, "retained_source": "pi source", "pane": "p1"}
        for harness, connection, allowed in (
            ("pi", "ready", True), ("qwen-pi", "ready", True),
            ("unknown", "ready", False), ("pi", "reconnecting", False),
        ):
            view = self.tray.presentation(
                {**base, "harness": harness, "connection_state": connection})
            self.assertEqual(view["actions"]["recover-stage"][1], allowed)
            self.assertIn("recover-copy", view["actions"])
            self.assertIn("recover-discard", view["actions"])
            self.assertIn("stop", view["actions"])
            self.assertNotIn("send", view["actions"])
            self.assertNotIn("append", view["actions"])
            self.assertIn("pi source", str(view["context"]))

    def test_retained_current_pi_requires_confirmation_before_staging(self):
        base = {
            "pane": "p1", "harness": "pi", "retained": True,
            "selection_explicit": False,
            "sessions": [{"token": "one", "id": "conversation",
                          "label": "Pi: notes", "selected": True}],
        }
        view = self.tray.presentation(base)
        self.assertEqual(view["actions"]["select:one"],
                         ("Confirm Pi: notes for retained dictation", True))
        self.assertFalse(view["actions"]["recover-stage"][1])
        confirmed = self.tray.presentation({**base, "selection_explicit": True})
        self.assertFalse(confirmed["actions"]["select:one"][1])
        self.assertTrue(confirmed["actions"]["recover-stage"][1])
        for changes in ({"phase": "recording"}, {"retained": False},
                        {"harness": "unknown"}):
            with self.subTest(changes=changes):
                self.assertFalse(self.tray.presentation({**base, **changes})[
                    "actions"]["select:one"][1])

    def test_confirmation_action_rechecks_current_conversation_identity(self):
        from unittest.mock import Mock
        old = {
            "pane": "p1", "harness": "pi", "retained": True,
            "selection_explicit": False,
            "sessions": [{"token": "one", "id": "original",
                          "label": "Pi: notes", "selected": True}],
        }
        for identity in ("original", "replacement"):
            with self.subTest(identity=identity):
                fresh = {
                    **old, "sessions": [{**old["sessions"][0], "id": identity}]}
                callback = Mock()
                tray = self.tray.VoiceTray(lambda: fresh, callback)
                self.addCleanup(tray.stop)
                tray.view = self.tray.presentation(old)
                tray.action("select:one")
                self.assertTrue(tray.action_lock.acquire(timeout=1))
                tray.action_lock.release()
                if identity == "original":
                    callback.assert_called_once_with("select:one")
                else:
                    callback.assert_not_called()

    def test_loading_and_reconnecting_allow_stop_not_record_more(self):
        view = self.tray.presentation(
            {"pane": "p1", "connection_state": "reconnecting", "draft": True})
        self.assertEqual(view["label"], "Reconnecting")
        self.assertNotIn("append", view["actions"])
        self.assertTrue(view["actions"]["stop"][1])
        view = self.tray.presentation(
            {
                "pane": "new",
                "session_label": "new",
                "recording_label": "pinned",
                "phase": "recording",
                "preparing_transcription": True,
            }
        )
        self.assertEqual(view["label"], "Preparing transcription")
        self.assertIn("pinned", str(view["context"]))
        self.assertTrue(view["actions"]["stop"][1])

    def test_starting_does_not_claim_recording_or_allow_send(self):
        view = self.tray.presentation({"pane": "p1", "phase": "starting"})
        self.assertEqual(view["label"], "Starting microphone")
        self.assertEqual(view["colour"], "amber")
        self.assertNotIn("send", view["actions"])
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
        self.assertNotIn("record", view["actions"])
        self.assertNotIn("send", view["actions"])
        self.assertNotIn("read", view["actions"])
        self.assertEqual(view["actions"]["stop"], ("Cancel recording", True))

    def test_empty_idle_and_busy_states_cannot_submit(self):
        for phase in ("idle", "starting", "stopping", "transcribing", "error"):
            with self.subTest(phase=phase):
                view = self.tray.presentation({"pane": "p1", "phase": phase})
                self.assertNotIn("send", view["actions"])

    def test_record_more_requires_prepared_text_and_a_selected_session(self):
        for pane in (None, "p1"):
            view = self.tray.presentation(
                {"pane": pane, "phase": "draft", "pending": True}
            )
            self.assertEqual("append" in view["actions"], bool(pane))

    def test_cancel_is_not_offered_for_text_already_staged_in_terminal(self):
        view = self.tray.presentation(
            {"pane": "p1", "phase": "draft", "draft": True}
        )
        self.assertEqual(view["actions"]["append"], ("Record more", True))
        self.assertNotIn("stop", view["actions"])

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
        state = {"pane": "p1", "phase": "draft", "draft": True}
        began, release, cancelled = (threading.Event() for _ in range(3))

        def action(name):
            if name == "append":
                state["phase"] = "starting"
                began.set()
                release.wait(2)
            if name == "stop":
                cancelled.set()

        tray = self.tray.VoiceTray(lambda: dict(state), action)
        try:
            tray.action("append")
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
            "pane": "p1", "harness": "Pi", "session_label": "dotfiles",
            "models": "loading", "microphone": {
                "name": "USB microphone", "muted": True,
                "preferred": "headset", "missing": True,
            },
        })
        context = " ".join(view["context"])
        self.assertIn("Pi: dotfiles", context)
        self.assertIn("USB microphone", context)
        self.assertIn("muted", context)
        self.assertIn("preferred microphone unavailable", context)
        self.assertIn("Speech models loading", context)
        self.assertEqual(view["colour"], "amber")

    def test_idle_menu_has_only_the_read_replies_toggle_and_sessions(self):
        view = self.tray.presentation({"pane": "p1", "auto": True})
        self.assertEqual(view["actions"], {
            "auto-toggle": ("Read replies aloud", True),
            "team-toggle": ("Show team members", True),
        })
        self.assertTrue(view["auto"])

    def test_voice_choices_are_characters_without_samantha_variants(self):
        voices = {"samantha": "Samantha", "data": "Data", "jarvis": "JARVIS"}
        for character in voices:
            view = self.tray.presentation(
                {"selected_voice": character, "voices": voices}
            )
            self.assertEqual(view["selected_voice"], character)
            self.assertEqual(
                {key for key in view["actions"] if key.startswith("voice:")},
                {"voice:" + key for key in voices},
            )

    def test_dictation_choices_are_exclusive_and_lock_while_busy(self):
        backends = {
            "whisper": "Whisper (local GPU)",
            "deepgram": "Deepgram (cloud)",
        }
        idle = self.tray.presentation({
            "selected_stt": "whisper", "stt_backends": backends,
        })
        self.assertEqual(idle["selected_stt"], "whisper")
        self.assertEqual(
            idle["actions"]["stt:whisper"], ("Whisper (local GPU)", True)
        )
        self.assertEqual(
            idle["actions"]["stt:deepgram"], ("Deepgram (cloud)", True)
        )
        self.assertIn("Dictation: Whisper", idle["context"])
        busy = self.tray.presentation({
            "phase": "recording", "selected_stt": "deepgram",
            "stt_backends": backends,
        })
        self.assertEqual(busy["selected_stt"], "deepgram")
        self.assertFalse(busy["actions"]["stt:whisper"][1])
        self.assertFalse(busy["actions"]["stt:deepgram"][1])
        self.assertIn("Dictation: Deepgram", busy["context"])

    def test_child_silence_hides_replay_without_disabling_dictation(self):
        for child in (False, True):
            with self.subTest(child=child):
                view = self.tray.presentation({
                    'pane': 'p1', 'harness': 'pi', 'reply': 'Summary',
                    'can_speak': not child, 'auto': True,
                    'retained': True, 'selection_explicit': True,
                    'sessions': [{'token': 'child', 'selected': True,
                                  'team_child': child}],
                })
                self.assertEqual('read' in view['actions'], not child)
                self.assertEqual('Team members are silent' in view['context'],
                                 child)
                self.assertTrue(view['auto'])
                self.assertTrue(view['actions']['recover-stage'][1])

    def test_last_reply_can_be_replayed_only_when_usable(self):
        status = {"pane": "p1", "reply": "An answer"}
        view = self.tray.presentation(status)
        self.assertEqual(view["actions"]["read"], ("Replay last reply", True))
        for updates in ({"speaking": True}, {"responding": True},
                        {"phase": "recording"}, {"pending": True}):
            self.assertNotIn("read", self.tray.presentation(
                {**status, **updates}
            )["actions"])

    def test_responding_has_blue_dots_and_blocked_has_a_distinct_glyph(self):
        responding = self.tray.presentation({
            "pane": "p1", "harness": "Pi", "responding": True,
            "agent_state": "working",
        })
        self.assertEqual(responding["label"], "Pi is responding")
        self.assertEqual((responding["colour"], responding["glyph"]),
                         ("blue", "dots"))
        self.assertNotIn("stop", responding["actions"])
        blocked = self.tray.presentation({
            "pane": "p1", "harness": "Pi", "agent_state": "blocked",
        })
        self.assertIn("needs your attention", blocked["label"])
        self.assertEqual(blocked["glyph"], "blocked")

    def test_voice_work_takes_priority_over_agent_activity(self):
        for state, colour, glyph in (
            ({"phase": "recording"}, "red", "microphone"),
            ({"phase": "transcribing"}, "amber", "microphone"),
            ({"phase": "draft", "draft": True}, "green", "check"),
            ({"phase": "error", "error": "Lost microphone"},
             "orange", "blocked"),
        ):
            view = self.tray.presentation({
                "pane": "p1", "responding": True, **state,
            })
            self.assertEqual((view["colour"], view["glyph"]), (colour, glyph))

    def test_icons_have_distinct_shapes_even_without_colour(self):
        icons = [self.tray.icon_pixmap("blue", glyph=glyph)[0][2]
                 for glyph in ("microphone", "dots", "blocked",
                               "speaker", "check")]
        self.assertEqual(len(set(icons)), 5)

    def test_cancel_labels_describe_the_voice_operation(self):
        for status, label in (
            ({"phase": "recording"}, "Cancel recording"),
            ({"phase": "transcribing"}, "Cancel transcription"),
            ({"speaking": True}, "Stop speaking"),
        ):
            view = self.tray.presentation({"pane": "p1", **status})
            self.assertEqual(view["actions"]["stop"], (label, True))

    def test_three_dots_are_visible_at_the_icon_centres(self):
        pixels = self.tray.icon_pixmap("blue", glyph="dots")[0][2]

        def pixel(x, y):
            start = (y * 32 + x) * 4
            return tuple(pixels[start:start + 4])

        for x in (8, 16, 24):
            self.assertEqual(pixel(x, 16), (255, 255, 255, 255))
        self.assertEqual(pixel(16, 10), (255, 48, 125, 224))

    def test_retained_text_offers_only_relevant_recovery_actions(self):
        view = self.tray.presentation({
            "pane": "p1", "phase": "draft", "pending": True,
            "harness": "Pi", "retry": True,
        })
        self.assertIn("Pi", view["label"])
        for name in ("append", "discard", "retry"):
            self.assertTrue(view["actions"][name][1])
        fresh = self.tray.presentation({"pane": "p1"})
        self.assertNotIn("append", fresh["actions"])
        self.assertNotIn("replace", fresh["actions"])
        self.assertNotIn("retry", fresh["actions"])

    def test_retained_dictation_disables_read_until_sent_or_discarded(self):
        view = self.tray.presentation({
            "pane": "p1", "phase": "draft", "pending": True,
            "reply": "Latest reply",
        })
        self.assertNotIn("read", view["actions"])

    def test_retry_audio_has_one_discard_action_without_pending_text(
        self,
    ):
        view = self.tray.presentation({
            "pane": "p1", "phase": "error", "retry": True,
        })
        self.assertNotIn("stop", view["actions"])
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
                {"token": "one", "label": "Pi: dotfiles",
                 "selected": True},
                {"token": "two", "label": "Pi: notes", "selected": False},
            ],
        })
        self.assertNotIn("rebind", view["actions"])
        self.assertFalse(view["actions"]["select:one"][1])
        self.assertTrue(view["actions"]["select:two"][1])
        self.assertEqual(view["actions"]["select:one"][0], "Pi: dotfiles")
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
        source = MODULE.with_name("voice_controller.py")
        spec = importlib.util.spec_from_file_location(
            "voice_controller", source)
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
            "voice_tray", MODULE
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
            "microphone": {"name": "USB mic"}, "auto": True,
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
            if name == "auto-toggle":
                state["auto"] = not state["auto"]
            if name.startswith("voice:"):
                state["selected_voice"] = name.split(":", 1)[1]
            if name.startswith("stt:"):
                state["selected_stt"] = name.split(":", 1)[1]
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
                labels = [row[1]["label"].value for row in rows]
                self.assertEqual(
                    labels,
                    [
                        "Voice session",
                        "Read replies aloud",
                        "Show team members",
                        "Cancel recording",
                    ],
                )
                tooltip = (await item.get_tool_tip())[3]
                for detail in ("00:12", "Pi: dotfiles", "Microphone: USB mic",
                               "Speech models loading"):
                    self.assertIn(detail, tooltip)
                # A stale Send row from the old tray cannot trigger delivery.
                await menu.call_event(3, "clicked", Variant("i", 0), 0)
                await asyncio.sleep(0.1)
                self.assertEqual(actions, [])
                cancel = next(
                    row
                    for row in rows
                    if row[1]["label"].value == "Cancel recording"
                )
                await menu.call_event(cancel[0], "clicked", Variant("i", 0), 0)
                await wait_until(lambda: actions == ["stop"])
                # A slow controller action must not stall tray D-Bus replies.
                self.assertEqual(
                    await asyncio.wait_for(item.get_title(), 0.5), "Agent Voice"
                )
                action_release.set()
                state.update(phase="idle", models="ready", responding=True)
                await wait_until(lambda: tray.view.get("glyph") == "dots")
                working_pixmap = await item.get_icon_pixmap()
                icon_count = len(icons)
                state.update(responding=False, speaking=True)
                await wait_until(lambda: len(icons) > icon_count)
                self.assertEqual(tray.view["glyph"], "speaker")
                self.assertNotEqual(
                    await item.get_icon_pixmap(), working_pixmap
                )
                state["speaking"] = False
                state.update(phase="idle", sessions=[
                    {"token": "first", "label": "pi: dotfiles",
                     "selected": True},
                    {"token": "second", "label": "qwen-pi: notes"},
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
                    ["pi: dotfiles", "qwen-pi: notes"],
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
                    if row[1]["label"].value == "qwen-pi: notes"
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
                    {"token": "third", "label": "pi: third"},
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
                _, layout = await menu.call_get_layout(0, -1, [])
                toggle = next(
                    child.value for child in layout[2]
                    if child.value[1]["label"].value == "Read replies aloud"
                )
                self.assertEqual(toggle[1]["toggle-type"].value, "checkmark")
                self.assertEqual(toggle[1]["toggle-state"].value, 1)
                await menu.call_event(
                    toggle[0], "clicked", Variant("i", 0), 0
                )
                await wait_until(lambda: tray.view.get("auto") is False)
                _, updated_toggle = await menu.call_get_layout(toggle[0], 0, [])
                self.assertEqual(updated_toggle[1]["toggle-state"].value, 0)
                state.update(
                    voices={
                        "samantha": "Samantha",
                        "data": "Data",
                        "jarvis": "JARVIS",
                    },
                    selected_voice="samantha",
                )
                await wait_until(lambda: "voice:jarvis" in tray.view["actions"])
                _, layout = await menu.call_get_layout(0, -1, [])
                selector = next(
                    child.value
                    for child in layout[2]
                    if child.value[1]["label"].value == "Character voice"
                )
                voices = [child.value for child in selector[2]]
                self.assertEqual(
                    [r[1]["toggle-state"].value for r in voices], [1, 0, 0]
                )
                self.assertTrue(
                    all(r[1]["toggle-type"].value == "radio" for r in voices)
                )
                before = list(actions)
                await menu.call_event(
                    selector[0], "clicked", Variant("i", 0), 0
                )
                await asyncio.sleep(0.1)
                self.assertEqual(actions, before)
                await menu.call_event(
                    voices[2][0], "clicked", Variant("i", 0), 0
                )
                await wait_until(
                    lambda: tray.view["selected_voice"] == "jarvis"
                )
                _, subtree = await menu.call_get_layout(selector[0], -1, [])
                self.assertEqual(
                    [r.value[1]["toggle-state"].value for r in subtree[2]],
                    [0, 0, 1],
                )
                state.update(
                    stt_backends={
                        "whisper": "Whisper (local GPU)",
                        "deepgram": "Deepgram (cloud)",
                    },
                    selected_stt="whisper",
                )
                await wait_until(
                    lambda: "stt:deepgram" in tray.view["actions"]
                )
                _, layout = await menu.call_get_layout(0, -1, [])
                selector = next(
                    child.value
                    for child in layout[2]
                    if child.value[1]["label"].value == "Dictation"
                )
                backends = [child.value for child in selector[2]]
                self.assertEqual(
                    [row[1]["label"].value for row in backends],
                    ["Whisper (local GPU)", "Deepgram (cloud)"],
                )
                self.assertEqual(
                    [row[1]["toggle-state"].value for row in backends], [1, 0]
                )
                self.assertTrue(all(
                    row[1]["toggle-type"].value == "radio"
                    for row in backends
                ))
                before = list(actions)
                await menu.call_event(
                    selector[0], "clicked", Variant("i", 0), 0
                )
                await asyncio.sleep(0.1)
                self.assertEqual(actions, before)
                await menu.call_event(
                    backends[1][0], "clicked", Variant("i", 0), 0
                )
                await wait_until(
                    lambda: tray.view["selected_stt"] == "deepgram"
                )
                _, subtree = await menu.call_get_layout(selector[0], -1, [])
                self.assertEqual(
                    [row.value[1]["toggle-state"].value for row in subtree[2]],
                    [0, 1],
                )
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
