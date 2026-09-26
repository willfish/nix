"""Tray presentation never offers delivery for incomplete or empty audio."""

import importlib.util
import os
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest

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

    def test_cancel_labels_describe_the_voice_operation(self):
        for status, label in (
            ({"phase": "recording"}, "Cancel recording"),
            ({"phase": "transcribing"}, "Cancel transcription"),
            ({"speaking": True}, "Stop speaking"),
        ):
            view = self.tray.presentation({"pane": "p1", **status})
            self.assertEqual(view["actions"]["stop"], (label, True))

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
            worker = threading.Thread(
                target=controller.stage, args=("Pending words", "one"),
                daemon=True,
            )
            try:
                worker.start()
                self.assertTrue(entered.wait(1))
                controller.stop()
                self.assertTrue(stopped.wait(0.2))
                self.assertTrue(controller.input_cancelled.is_set())
            finally:
                release.set()
                worker.join(1)
            self.assertFalse(controller.status()["draft"])
            self.assertFalse(controller.status()["pending"])
