"""Agent activity never blocks voice controls or trusts stale probes."""

import importlib.util
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SOURCE = (
    Path(__file__).resolve().parents[1] / \
         "home/config/voice/voice_controller.py"
)
spec = importlib.util.spec_from_file_location(
    "voice_activity_controller", SOURCE
)
voice = importlib.util.module_from_spec(spec)
spec.loader.exec_module(voice)


class ActivityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.entered, self.release = threading.Event(), threading.Event()
        self.addCleanup(self.release.set)
        self.probes = []

        def activity(target):
            self.probes.append(target["pane"])
            self.entered.set()
            self.release.wait(2)
            return "working"

        self.terminal = SimpleNamespace(activity=activity)
        self.app = voice.Controller(
            Path(self.directory.name), self.terminal,
            SimpleNamespace(stop=lambda: None), lambda *args: None,
        )
        self.target = {
            "harness": "pi", "pane": "first", "pid": 1, "start": "2",
        }
        self.app.register("first", self.target, "conversation-first")

    def wait_for(self, condition):
        deadline = time.monotonic() + 1
        while not condition():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)

    def test_slow_probe_cannot_block_status_or_cancel(self):
        started = time.monotonic()
        initial = self.app.status()
        self.assertLess(time.monotonic() - started, 0.1)
        self.assertEqual(initial["agent_state"], "unknown")
        self.assertFalse(initial["responding"])
        self.assertTrue(self.entered.wait(1))
        for _ in range(5):
            self.app.status()
        self.assertEqual(self.probes, ["first"])
        started = time.monotonic()
        self.app.stop()
        self.assertLess(time.monotonic() - started, 0.1)
        self.release.set()
        self.wait_for(lambda: self.app.status()["responding"])

    def test_adapter_states_are_normalized_without_input_delivery(self):
        terminal = voice.AgentTerminal()
        terminal.herdr = Mock()
        terminal.pi = Mock()
        with patch.object(voice, "process_start", return_value="2"):
            for harness, raw, expected in (
                ("pi", "working", "working"),
                ("pi", "done", "idle"),
                ("qwen-pi", "blocked", "blocked"),
            ):
                with self.subTest(harness=harness, state=raw):
                    terminal.pi.validate_target.return_value = {"state": raw}
                    self.assertEqual(terminal.activity(dict(
                        self.target, harness=harness
                    )), expected)
                    terminal.pi.submit_guarded.assert_not_called()
                    terminal.pi.insert_guarded.assert_not_called()

    def test_native_activity_tracks_selection_and_blocked_state(self):
        del self.terminal.activity
        self.app.register(
            "second", dict(self.target, pane="second", harness="qwen-pi"),
            "conversation-second",
        )
        event = {
            "harness": "pi", "session": "conversation-first", "type": "busy",
        }
        self.app.harness_event("first", event)
        self.assertFalse(self.app.status()["responding"])
        self.app.select("first")
        self.assertTrue(self.app.status()["responding"])
        self.app.harness_event("first", dict(event, state="blocked"))
        self.assertEqual(self.app.status()["agent_state"], "blocked")
        self.assertFalse(self.app.status()["responding"])
        self.app.harness_event("first", dict(event, type="settled"))
        self.assertEqual(self.app.status()["agent_state"], "blocked")
        self.terminal.activity = lambda target: "idle"
        self.wait_for(lambda: self.app.status()["agent_state"] == "idle")

    def test_old_probe_cannot_override_a_new_native_event(self):
        self.app.status()
        self.assertTrue(self.entered.wait(1))
        self.app.harness_event("first", {
            "harness": "pi", "session": "conversation-first",
            "type": "busy", "state": "blocked",
        })
        self.assertEqual(self.app.status()["agent_state"], "blocked")
        self.release.set()
        self.wait_for(lambda: not self.app.activity_inflight)
        self.assertEqual(self.app.status()["agent_state"], "blocked")

    def test_late_settled_event_cannot_clear_a_newer_turn(self):
        del self.terminal.activity
        self.app.register(
            "first", dict(self.target, harness="pi"), "conversation-first"
        )
        event = {"harness": "pi", "session": "conversation-first"}
        self.app.harness_event("first", dict(
            event, type="busy", turn="new-turn"
        ))
        self.app.harness_event("first", dict(
            event, type="settled", turn="old-turn"
        ))
        self.assertTrue(self.app.status()["responding"])
        self.app.harness_event("first", dict(
            event, type="settled", turn="new-turn"
        ))
        self.assertEqual(self.app.status()["agent_state"], "idle")

    def test_late_reply_keeps_new_activity_and_existing_reply_handling(self):
        del self.terminal.activity
        self.app.register(
            "first", dict(self.target, harness="pi"), "conversation-first"
        )
        self.app.harness_event("first", {
            "harness": "pi", "session": "conversation-first",
            "type": "busy", "turn": "new-turn",
        })
        self.assertTrue(self.app.notify("first", {
            "type": "agent-turn-complete", "thread-id": "conversation-first",
            "turn-id": "old-turn",
            "last-assistant-message": "TL;DR: Earlier reply.",
        }))
        self.assertTrue(self.app.status()["responding"])
        self.assertEqual(self.app.reply, "Earlier reply.")

    def test_late_background_reply_cannot_clear_that_sessions_newer_turn(self):
        del self.terminal.activity
        self.app.register(
            "first", dict(self.target, harness="pi"), "conversation-first"
        )
        self.app.harness_event("first", {
            "harness": "pi", "session": "conversation-first",
            "type": "busy", "turn": "new-turn",
        })
        self.app.register("second", dict(self.target, pane="second"))
        self.app.notify("first", {
            "type": "agent-turn-complete", "thread-id": "conversation-first",
            "turn-id": "old-turn",
            "last-assistant-message": "TL;DR: Earlier reply.",
        })
        self.app.select("first")
        self.assertTrue(self.app.status()["responding"])
        self.assertEqual(self.app.reply, "Earlier reply.")

    def assert_unordered_completion_waits_for_probe(self, completion,
                                                  harness="pi"):
        new_entered, new_release = threading.Event(), threading.Event()
        self.addCleanup(new_release.set)

        def activity(target):
            self.probes.append(target["pane"])
            if len(self.probes) == 1:
                self.entered.set()
                self.release.wait(2)
            else:
                new_entered.set()
                new_release.wait(2)
            return "idle"

        self.terminal.activity = activity
        if harness != "pi":
            self.app.register(
                "first", dict(self.target, harness=harness),
                "conversation-first",
            )
        self.app.harness_event("first", {
            "harness": harness, "session": "conversation-first",
            "type": "busy",
            "turn": "new-turn" if harness == "qwen-pi" else None,
        })
        self.assertTrue(self.app.status()["responding"])
        self.assertTrue(self.entered.wait(1))
        completion()
        self.assertTrue(self.app.status()["responding"])
        self.release.set()
        self.wait_for(lambda: not self.app.activity_inflight)
        self.assertTrue(self.app.status()["responding"])
        self.assertTrue(new_entered.wait(1))
        new_release.set()
        self.wait_for(lambda: self.app.status()["agent_state"] == "idle")
        self.assertEqual(self.probes, ["first", "first"])

    def test_untagged_settled_invalidates_probe_and_confirms_live_state(self):
        self.assert_unordered_completion_waits_for_probe(
            lambda: self.app.harness_event("first", {
                "harness": "pi", "session": "conversation-first",
                "type": "settled",
            })
        )

    def test_pi_reply_without_a_shared_run_id_requires_confirmation(self):
        self.assert_unordered_completion_waits_for_probe(
            lambda: self.app.notify("first", {
                "type": "agent-turn-complete",
                "thread-id": "conversation-first", "turn-id": "leaf-id",
                "last-assistant-message": "TL;DR: Completed reply.",
            })
        )
        self.assertEqual(self.app.reply, "Completed reply.")

    def test_untagged_settled_cannot_clear_a_known_new_turn(self):
        self.assert_unordered_completion_waits_for_probe(
            lambda: self.app.harness_event("first", {
                "harness": "qwen-pi", "session": "conversation-first",
                "type": "settled",
            }), harness="qwen-pi",
        )

    def test_background_probe_cannot_mark_another_session_as_responding(self):
        self.app.status()
        self.assertTrue(self.entered.wait(1))
        self.app.register("second", dict(self.target, pane="second"))
        self.terminal.activity = lambda target: "idle"
        self.app.status()
        self.wait_for(lambda: self.app.status()["agent_state"] == "idle")
        self.release.set()
        self.wait_for(lambda: not self.app.activity_inflight)
        self.assertFalse(self.app.status()["responding"])

    def test_probe_failure_reports_unknown_without_breaking_voice_status(self):
        self.terminal.activity = Mock(side_effect=RuntimeError("Disconnected"))
        self.app.status()
        self.wait_for(lambda: not self.app.activity_inflight)
        self.assertEqual(self.app.status()["agent_state"], "unknown")
        self.assertIsNone(self.app.status()["error"])

    def test_rebinding_drops_old_conversation_activity(self):
        del self.terminal.activity
        self.app.harness_event("first", {
            "harness": "pi", "session": "conversation-first", "type": "busy",
        })
        self.app.harness_event("first", {
            "harness": "pi", "session": "conversation-new", "type": "session",
        })
        self.app.rebind()
        self.assertEqual(self.app.status()["agent_state"], "unknown")

    def test_edited_draft_hides_but_preserves_explicit_send(self):
        self.app.draft = True
        self.app.phase = "draft"
        self.app.send_when_idle = True
        self.terminal.activity_snapshot = lambda target: {
            "state": "idle", "draft_state": "edited",
        }
        self.app.status()
        self.wait_for(lambda: not self.app.activity_inflight)
        status = self.app.status()
        self.assertTrue(status["draft"])
        self.assertTrue(status["draft_edited"])
        self.assertFalse(status["queued"])

    def test_cleared_editor_dismisses_draft_without_touching_pending_text(self):
        for pending in (None, "New undelivered dictation"):
            with self.subTest(pending=bool(pending)):
                self.app.draft = True
                self.app.pending = pending
                self.app.phase = "draft"
                self.app.send_when_idle = True
                self.app.sessions["first"]["activity_checked"] = float("-inf")
                self.terminal.activity_snapshot = lambda target: {
                    "state": "idle", "draft_state": "empty",
                }
                self.app.status()
                self.wait_for(lambda: not self.app.activity_inflight)
                status = self.app.status()
                self.assertFalse(status["draft"])
                self.assertTrue(status["draft_edited"])
                self.assertEqual(self.app.pending, pending)
                self.assertEqual(status["queued"], bool(pending))
                self.assertEqual(
                    status["phase"], "draft" if pending else "idle"
                )

    def test_old_editor_probe_cannot_dismiss_a_newly_staged_draft(self):
        def snapshot(target):
            self.entered.set()
            self.release.wait(2)
            return {"state": "idle", "draft_state": "empty"}

        self.terminal.activity_snapshot = snapshot
        self.terminal.insert = lambda target, text: None
        self.app.draft = True
        self.app.phase = "draft"
        self.app.status()
        self.assertTrue(self.entered.wait(1))
        self.assertTrue(self.app.stage("New dictation", "first"))
        self.release.set()
        self.wait_for(lambda: not self.app.activity_inflight)
        self.assertTrue(self.app.draft)
        self.assertEqual(self.app.sessions["first"]["draft_state"], "staged")

    def test_missing_or_failed_editor_snapshot_does_not_discard_a_draft(self):
        for response in ({"state": "idle"}, None):
            with self.subTest(response=response):
                self.app.draft = True
                self.app.sessions["first"]["activity_checked"] = float("-inf")
                self.terminal.activity_snapshot = lambda target: response
                self.app.status()
                self.wait_for(lambda: not self.app.activity_inflight)
                self.assertTrue(self.app.draft)

    def test_tray_can_toggle_automatic_reading(self):
        del self.terminal.activity
        self.assertTrue(voice.dispatch(
            self.app, {"action": "auto-toggle"}
        )["auto"])
        self.assertFalse(voice.dispatch(
            self.app, {"action": "auto-toggle"}
        )["auto"])


if __name__ == "__main__":
    unittest.main()
