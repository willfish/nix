"""The dictation card is public state for the selected session only."""

import importlib.util
import os
from pathlib import Path
import unittest


MODULE = Path(os.environ.get(
    "VOICE_TEST_OSD",
    Path(__file__).resolve().parents[1] / "home/config/voice/voice_osd.py",
))


class OsdTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("voice_osd", MODULE)
        self.osd = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.osd)

    def test_hidden_until_super_space_or_recording(self):
        idle = self.osd.osd_view({"phase": "idle", "pane": "w1:p1"})
        self.assertFalse(idle["visible"])
        shown = self.osd.osd_view({
            "phase": "idle",
            "osd": True,
            "pane": "w1:p1",
            "harness": "pi",
            "sessions": [{
                "selected": True,
                "full_label": "pi · notes · selected",
            }],
        })
        self.assertTrue(shown["visible"])
        self.assertEqual(shown["detail"], "notes")

    def test_recording_names_the_selected_session_and_level(self):
        view = self.osd.osd_view({
            "phase": "recording",
            "recording_seconds": 65,
            "input_level": 1.4,
            "recording_label": "pi · tariff",
            "pending": "secret dictated text",
            "reply": "secret reply",
        })
        self.assertTrue(view["visible"])
        self.assertEqual(view["title"], "Listening 01:05")
        self.assertEqual(view["detail"], "tariff")
        self.assertEqual(view["meter"][-1], "█")
        self.assertEqual(view["level"], 1.0)
        rendered = " ".join(str(value) for value in view.values())
        self.assertNotIn("secret", rendered)

    def test_failure_shows_the_reason_not_a_transcript(self):
        view = self.osd.osd_view({
            "phase": "idle",
            "osd": True,
            "osd_message": "Select a Pi voice session first",
            "pending": "do not show this",
        })
        self.assertEqual(view["title"], "Voice unavailable")
        self.assertEqual(view["detail"], "Select a Pi voice session first")
        self.assertNotIn(
            "do not show", " ".join(str(value) for value in view.values())
        )

    def test_session_line_drops_model_and_selection_flags(self):
        view = self.osd.osd_view({
            "phase": "recording",
            "recording_label": (
                "pi · dot · 5 dictation like omarchy · medium · "
                "grok-4.7 · selected"
            ),
        })
        self.assertEqual(view["detail"], "dot · 5 dictation like omarchy")
        self.assertEqual(self.osd.level_to_block(0), "▁")
        self.assertEqual(self.osd.level_to_block(1), "█")
        self.assertEqual(len(self.osd.meter_blocks([0.4])), 8)
        self.assertTrue(self.osd.meter_blocks([1]).endswith("█"))

    def test_no_session_is_explicit(self):
        view = self.osd.osd_view({"phase": "starting", "osd": True})
        self.assertEqual(view["detail"], "No Pi session selected")

    def test_focused_connector_requires_one_focused_monitor(self):
        self.assertEqual(self.osd.focused_connector([
            {"name": "DP-1", "focused": True},
            {"name": "HDMI-A-1", "focused": False},
        ]), "DP-1")
        self.assertIsNone(self.osd.focused_connector([
            {"name": "DP-1", "focused": True},
            {"name": "HDMI-A-1", "focused": True},
        ]))

    def test_status_socket_drops_the_ok_flag_and_keeps_public_fields(self):
        status = self.osd.status_from_response({
            "ok": True,
            "phase": "recording",
            "reply": "secret reply",
        })
        self.assertEqual(status["phase"], "recording")
        self.assertNotIn("ok", status)
        self.assertIsNone(self.osd.status_from_response({"ok": False}))

    def test_popup_colours_follow_the_active_theme(self):
        colours = self.osd.popup_colours(
            "[popups]\nbackground = \"#111111\"\ntext = \"#eeeeee\"\n"
            "border = \"#abcdef\"\n"
        )
        self.assertEqual(colours["background"], "#111111")
        self.assertEqual(colours["text"], "#eeeeee")
        self.assertEqual(colours["accent"], "#abcdef")


if __name__ == "__main__":
    unittest.main()
