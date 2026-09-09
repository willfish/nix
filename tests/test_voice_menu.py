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
