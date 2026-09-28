import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch

SCRIPTS = Path(__file__).resolve().parents[1] / "home/config/voice"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location(
    "voice_menu", SCRIPTS / "voice_menu.py"
)
menu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(menu)


def status():
    return {
        "pane": "pane-a",
        "harness": "pi",
        "phase": "idle",
        "auto": True,
        "voices": {"samantha": "Samantha", "data": "Data"},
        "selected_voice": "samantha",
        "sessions": [
            {
                "token": "a",
                "id": "thread-a",
                "label": "same label",
                "selected": True,
            },
            {
                "token": "b",
                "id": "thread-b",
                "label": "same label",
                "selected": False,
            },
        ],
    }


class MenuTests(unittest.TestCase):
    def retained_pi(self):
        from test_pi_voice_controller import ManagedTests
        fixture = ManagedTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        target = fixture.target()
        token = fixture.attach(target)
        fixture.event(token, target)
        fixture.app.retain_dictation("retained words", token, target)
        return fixture, token, target

    def test_sole_autoselected_pi_can_confirm_then_stage_without_submit(self):
        import voice_controller as voice
        fixture, token, _ = self.retained_pi()
        before = fixture.app.status()
        self.assertFalse(before["selection_explicit"])
        self.assertEqual(len(before["sessions"]), 1)
        self.assertNotIn("recover-stage", dict(menu.rows_for(before, "menu")))
        shown = menu.rows_for(before, "sessions")
        self.assertEqual([action for action, _ in shown], [f"select:{token}"])
        self.assertTrue(shown[0][1].startswith("* Confirm "))
        self.assertTrue(shown[0][1].endswith(" for retained dictation"))
        self.assertIn("w1:p1", shown[0][1])
        request = lambda message: voice.dispatch(fixture.app, message)
        menu.run_menu("sessions", request=request,
                      picker=Mock(return_value=f"select:{token}"))
        self.assertTrue(fixture.app.status()["selection_explicit"])
        self.assertEqual(fixture.terminal.text, [])
        self.assertEqual(fixture.terminal.keys, [])
        self.assertIn("recover-stage",
                      dict(menu.rows_for(fixture.app.status(), "menu")))
        menu.run_menu(request=request, picker=Mock(
            return_value="recover-stage"))
        self.assertEqual(fixture.terminal.text, ["retained words"])
        self.assertEqual(fixture.terminal.keys, [])

    def test_current_confirmation_rejects_reloaded_conversation(self):
        fixture, token, target = self.retained_pi()
        before = fixture.app.status()
        replacement = fixture.target(session="replacement", activation=2)
        replacement_token = fixture.attach(replacement)
        fixture.event(replacement_token, replacement)
        fresh = fixture.app.status()
        self.assertNotEqual(before["sessions"][0]["id"],
                            fresh["sessions"][0]["id"])
        request = Mock(side_effect=[before, fresh])
        with self.assertRaisesRegex(
            RuntimeError, "no longer available|session changed"
        ):
            menu.run_menu("sessions", request=request,
                          picker=Mock(return_value=f"select:{token}"))
        self.assertEqual(request.call_count, 2)
        self.assertFalse(fixture.app.selection_explicit)
        self.assertEqual(fixture.terminal.text, [])
        self.assertEqual(fixture.terminal.keys, [])

    def test_current_confirmation_rejects_changed_identity_with_same_token(
        self,
    ):
        old, fresh = status(), status()
        for state in (old, fresh):
            state.update(retained=True, selection_explicit=False)
        fresh["sessions"][0]["id"] = "replacement"
        request = Mock(side_effect=[old, fresh])
        with self.assertRaisesRegex(RuntimeError, "session changed"):
            menu.run_menu("sessions", request=request,
                          picker=Mock(return_value="select:a"))
        self.assertEqual(request.call_count, 2)

    def test_replaced_conversation_with_same_token_cannot_be_selected(self):
        fresh = status()
        fresh["sessions"][1]["id"] = "replacement"
        request = Mock(side_effect=[status(), fresh])
        with self.assertRaisesRegex(RuntimeError, "session changed"):
            menu.run_menu("sessions", request=request,
                          picker=Mock(return_value="select:b"))
        self.assertEqual(request.call_count, 2)

    def test_label_refresh_does_not_change_identity(self):
        fresh = status()
        fresh["sessions"][1]["label"] = "renamed"
        request = Mock(side_effect=[status(), fresh, {}])
        menu.run_menu("sessions", request=request,
                      picker=Mock(return_value="select:b"))
        self.assertEqual(request.call_args.args[0], {"action": "select:b"})

    def test_team_toggle_and_hidden_rows(self):
        state = status()
        state["sessions"][1]["team_child"] = True
        self.assertEqual(
            [action for action, _ in menu.rows_for(state, "sessions")],
            ["select:a"],
        )
        self.assertIn(("team-toggle", "Show team members: off"),
                      menu.rows_for(state, "menu"))
        request = Mock(return_value=state)
        menu.run_menu(request=request, picker=Mock(return_value="team-toggle"))
        self.assertEqual(request.call_args.args[0], {"action": "team-toggle"})

    def test_selected_session_context_replaces_uuid_in_header(self):
        state = status()
        state['session_label'] = '01a0e6d4-6e91-7403-b822-e7357e8775b0'
        state['sessions'][0].update(
            full_label='pi · dot · 1 · medium · grok-4.7 · selected',
            thinking='medium', model='grok-4.7',
        )
        self.assertEqual(
            menu.prompt_for(state, 'menu'), 'Voice | dot · 1 · grok-4.7')
        self.assertEqual(
            dict(menu.rows_for(state, 'sessions'))['select:a'],
            '* dot · 1 · grok-4.7')
        request = Mock(return_value=state)
        menu.run_menu('sessions', request=request,
                      picker=Mock(return_value='select:a'))
        self.assertTrue(all(call.args[0] == {'action': 'status'}
                            for call in request.call_args_list))

    def test_named_session_keeps_workspace_pane_and_model(self):
        row = dict(
            full_label='pi · work · window · review · high · model · selected',
            thinking='high', selected=True,
        )
        self.assertEqual(menu.session_description(row),
                         'work · window · review · model')

    def test_selected_team_child_remains_visible_without_toggle(self):
        state = status()
        state["sessions"][0]["team_child"] = True
        shown = dict(menu.rows_for(state, "sessions"))
        self.assertTrue(shown["select:a"].startswith("* same label"))
        self.assertIn("select:b", shown)
        self.assertNotEqual(shown["select:a"], "* " + shown["select:b"])
        request = Mock(return_value=state)
        menu.run_menu("sessions", request=request,
                      picker=Mock(return_value="select:a"))
        self.assertTrue(
            all(
                call.args[0] == {"action": "status"}
                for call in request.call_args_list
            )
        )

    def test_recovery_stage_rechecks_selected_identity(self):
        old, fresh = status(), status()
        old["retained"] = fresh["retained"] = True
        fresh["sessions"][0]["id"] = "replacement"
        request = Mock(side_effect=[old, fresh])
        with self.assertRaisesRegex(RuntimeError, "session changed"):
            menu.run_menu(request=request, picker=Mock(
                return_value="recover-stage"))

    def test_stop_does_not_wait_for_fresh_status(self):
        state = {**status(), "connection_state": "connecting"}
        request = Mock(side_effect=[state, {}])
        menu.run_menu(request=request, picker=Mock(return_value="stop"))
        self.assertEqual(request.call_args_list[1].args[0], {"action": "stop"})

    def test_prompt_identifies_pinned_destination_and_connection(self):
        state = {
            **status(),
            "phase": "recording",
            "recording_label": "pinned destination",
            "session_label": "new destination",
        }
        picker = Mock(return_value=None)
        menu.run_menu(request=Mock(return_value=state), picker=picker)
        self.assertIn("pinned destination", picker.call_args.args[0])
        self.assertNotIn("new destination", picker.call_args.args[0])

    def test_cancel_only_reads_status(self):
        request = Mock(return_value=status())
        menu.run_menu(request=request, picker=Mock(return_value=None))
        request.assert_called_once_with({"action": "status"})

    def test_submenu_cancel_does_not_change_voice(self):
        request = Mock(return_value=status())
        menu.run_menu(
            request=request, picker=Mock(side_effect=["menu:voices", None])
        )
        self.assertTrue(
            all(
                c.args[0] == {"action": "status"}
                for c in request.call_args_list
            )
        )

    def test_voice_choice_uses_id(self):
        request = Mock(return_value=status())
        menu.run_menu(
            "voices", request=request, picker=Mock(return_value="voice:data")
        )
        self.assertEqual(request.call_args.args[0], {"action": "voice:data"})
        self.assertEqual(request.call_count, 3)

    def test_session_choice_uses_token_not_label(self):
        request = Mock(return_value=status())
        menu.run_menu(
            "sessions", request=request, picker=Mock(return_value="select:b")
        )
        self.assertEqual(request.call_args.args[0], {"action": "select:b"})

    def test_gone_session_is_not_selected(self):
        fresh = status()
        fresh["sessions"] = fresh["sessions"][:1]
        request = Mock(side_effect=[status(), fresh])
        with self.assertRaisesRegex(RuntimeError, "no longer available"):
            menu.run_menu(
                "sessions",
                request=request,
                picker=Mock(return_value="select:b"),
            )
        self.assertEqual(request.call_count, 2)

    def test_busy_sessions_are_not_offered(self):
        busy = status()
        busy["phase"] = "recording"
        self.assertEqual(menu.rows_for(busy, "sessions"), [])

    def test_busy_dictation_is_not_offered(self):
        busy = status()
        busy.update(
            phase="recording",
            selected_stt="whisper",
            stt_backends={
                "whisper": "Whisper (local GPU)",
                "deepgram": "Deepgram (cloud)",
            },
        )
        self.assertEqual(menu.rows_for(busy, "dictation"), [])

    def test_changed_target_cannot_receive_old_action(self):
        old, fresh = status(), status()
        old["reply"] = fresh["reply"] = "private reply"
        fresh["sessions"][0]["id"] = "different-thread"
        request = Mock(side_effect=[old, fresh])
        with self.assertRaisesRegex(RuntimeError, "session changed"):
            menu.run_menu(request=request, picker=Mock(return_value="read"))
        self.assertEqual(request.call_count, 2)

    def test_unavailable_action_is_rejected(self):
        old, fresh = status(), status()
        old["reply"] = "private reply"
        request = Mock(side_effect=[old, fresh])
        with self.assertRaisesRegex(RuntimeError, "no longer available"):
            menu.run_menu(request=request, picker=Mock(return_value="read"))

    def test_dictation_submenu_marks_the_selected_backend(self):
        state = status()
        state["selected_stt"] = "whisper"
        state["stt_backends"] = {
            "whisper": "Whisper (local GPU)",
            "deepgram": "Deepgram (cloud)",
        }
        self.assertIn(
            ("menu:dictation", "Choose dictation"),
            menu.rows_for(state, "menu"),
        )
        self.assertEqual(
            menu.rows_for(state, "dictation"),
            [
                ("stt:whisper", "* Whisper (local GPU)"),
                ("stt:deepgram", "Deepgram (cloud)"),
            ],
        )
        request = Mock(return_value=state)
        menu.run_menu(
            "dictation", request=request,
            picker=Mock(return_value="stt:deepgram"),
        )
        self.assertEqual(
            request.call_args.args[0], {"action": "stt:deepgram"},
        )

    def test_private_text_not_in_labels(self):
        state = status()
        state.update(draft="secret dictation", reply="secret response")
        labels = str(menu.rows_for(state, "menu"))
        self.assertNotIn("secret", labels)
        self.assertFalse(
            any(action == "send" for action, _ in menu.rows_for(state, "menu"))
        )

    def test_changed_auto_state_is_not_toggled(self):
        fresh = status()
        fresh["auto"] = False
        request = Mock(side_effect=[status(), fresh])
        with self.assertRaisesRegex(RuntimeError, "setting changed"):
            menu.run_menu(
                request=request, picker=Mock(return_value="auto-toggle")
            )

    def test_phase_controls_are_first_and_empty_launchers_are_omitted(self):
        recording = status()
        recording.update(
            phase="recording",
            stt_backends={"whisper": "Whisper", "deepgram": "Deepgram"},
        )
        rows = menu.rows_for(recording, "menu")
        self.assertEqual(rows[0], ("stop", "Cancel recording"))
        self.assertNotIn("menu:sessions", dict(rows))
        self.assertNotIn("menu:dictation", dict(rows))
        self.assertIn("menu:voices", dict(rows))

        ready = status()
        ready.update(draft=True, pending=True)
        self.assertEqual(menu.rows_for(ready, "menu")[0][0], "append")
        self.assertNotIn(
            "send", dict(menu.rows_for(ready, "menu"))
        )

        idle = status()
        idle["reply"] = "private reply"
        self.assertEqual(menu.rows_for(idle, "menu")[0][0], "read")

    def test_recovery_orders_confirmation_then_named_stage(self):
        held = status()
        held.update(retained=True, selection_explicit=False, harness="pi")
        self.assertEqual(menu.rows_for(held, "menu")[0][0], "menu:sessions")
        held["selection_explicit"] = True
        held["pending"] = True
        rows = dict(menu.rows_for(held, "menu"))
        self.assertTrue(rows["recover-stage"].startswith("Stage in same label"))
        self.assertEqual(rows["discard"], "Discard waiting dictation")
        self.assertEqual(
            rows["recover-discard"], "Discard unrecovered dictation"
        )
        self.assertNotIn("stop", rows)
        self.assertLess(
            list(rows).index("recover-stage"), list(rows).index("discard")
        )

    def test_prompt_hides_error_detail_and_names_missing_session(self):
        missing = {"phase": "idle"}
        self.assertEqual(menu.prompt_for(missing, "menu"), "Voice | No session")
        self.assertLessEqual(len(menu.prompt_for({
            "pane": "p", "retained": True,
            "retained_source": "pi \u00b7 " + ("workspace " * 8),
        }, "menu")), 28)
        shell = menu.prompt_for({
            "pane": "p", "phase": "recording",
            "recording_label": "pi \u00b7 trade-tariff-backend \u00b7 shell",
        }, "menu")
        review = menu.prompt_for({
            "pane": "p", "phase": "recording",
            "recording_label": "pi \u00b7 trade-tariff-backend \u00b7 review",
        }, "menu")
        self.assertNotEqual(shell, review)
        self.assertTrue(shell.endswith("shell"))
        self.assertTrue(review.endswith("review"))
        self.assertLessEqual(len(shell), 28)
        self.assertLessEqual(len(review), 28)
        failed = status()
        failed.update(phase="error", error="secret microphone fault")
        prompt = menu.prompt_for(failed, "menu")
        self.assertIn("Unavailable", prompt)
        self.assertNotIn("secret", prompt)
        self.assertNotIn("microphone", prompt)

    def test_locked_dictation_notice_differs_from_missing_backend(self):
        locked = status()
        locked.update(phase="recording", stt_backends={"whisper": "Whisper"})
        with self.assertRaisesRegex(RuntimeError, "locked"):
            menu.run_menu(
                "dictation", request=Mock(return_value=locked), picker=Mock()
            )
        with self.assertRaisesRegex(RuntimeError, "No dictation backends"):
            menu.run_menu(
                "dictation", request=Mock(return_value=status()), picker=Mock()
            )

    def test_colliding_session_labels_keep_tokens_and_tail(self):
        state = status()
        for row in state["sessions"]:
            row["selected"] = False
            row["label"] = ("alpha workspace " * 6) + " · pane-9"
        state["pane"] = None
        rows = menu.rows_for(state, "sessions")
        self.assertEqual(
            [action for action, _ in rows], ["select:a", "select:b"]
        )
        self.assertNotEqual(rows[0][1], rows[1][1])
        for _, label in rows:
            self.assertIn("pane-9", label)
            self.assertIn("\u2026", label)
            self.assertNotIn("secret", label)

    @patch.object(menu.subprocess, "run")
    def test_index_maps_original_rows_and_sanitizes_labels(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, "1\n", "")
        rows = [("select:a", "same\nlabel"), ("select:b", "same\tlabel")]
        self.assertEqual(menu.pick("Sessions", rows, "/config"), "select:b")
        args, kwargs = run.call_args
        self.assertIn("--only-match", args[0])
        self.assertIn("--index", args[0])
        self.assertEqual(kwargs["input"], "same label\nsame label\n")
        self.assertNotIn("shell", kwargs)

    @patch.object(menu.subprocess, "run")
    def test_custom_and_out_of_range_output_rejected(self, run):
        for output in ["-1", "9", "voice:data", "", "1\n0"]:
            with self.subTest(output=output):
                run.return_value = subprocess.CompletedProcess(
                    [], 0, output, ""
                )
                with self.assertRaises(RuntimeError):
                    menu.pick("Voice", [("voice:data", "Data")], "/config")

    @patch.object(menu.subprocess, "run")
    def test_escape_is_no_selection(self, run):
        run.return_value = subprocess.CompletedProcess([], 1, "", "")
        self.assertIsNone(
            menu.pick("Voice", [("voice:data", "Data")], "/config")
        )

    @patch.object(menu.subprocess, "run")
    def test_picker_failure_is_reported(self, run):
        run.return_value = subprocess.CompletedProcess(
            [], 1, "", "bad configuration"
        )
        with self.assertRaisesRegex(RuntimeError, "Fuzzel"):
            menu.pick("Voice", [("voice:data", "Data")], "/config")


if __name__ == "__main__":
    unittest.main()
