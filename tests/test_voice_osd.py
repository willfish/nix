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
        self.assertEqual(view["title"], "Select a Pi voice session first")
        self.assertEqual(view["tone"], "orange")
        self.assertNotEqual(view["detail"], "do not show this")
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

    def test_popup_colours_keep_explicit_state_roles(self):
        colours = self.osd.popup_colours(
            "[popups]\nred = \"#aa0000\"\nteal = \"#00aaaa\"\n"
            "accent = \"#0000aa\"\n"
        )
        self.assertEqual(colours["red"], "#aa0000")
        self.assertEqual(colours["teal"], "#00aaaa")
        self.assertEqual(colours["accent"], "#0000aa")

    def test_waybar_roles_fill_colours_the_popup_file_omits(self):
        colours = self.osd.resolved_colours(
            "[popups]\nborder = \"#111111\"\n",
            "@define-color red #aa0000;\n@define-color teal #00aaaa;\n"
            "@define-color accent #0000ff;\n",
        )
        self.assertEqual(colours["red"], "#aa0000")
        self.assertEqual(colours["teal"], "#00aaaa")
        self.assertEqual(colours["accent"], "#111111")

    def test_working_agent_stays_hidden_until_speech_is_queued(self):
        hidden = self.osd.osd_view({
            "phase": "idle",
            "pane": "w1:p1",
            "responding": True,
            "agent_state": "working",
        })
        self.assertFalse(hidden["visible"])
        imminent = self.osd.osd_view({
            "phase": "idle",
            "pane": "w1:p1",
            "speaking": True,
            "sessions": [{
                "selected": True,
                "full_label": "pi · notes · selected",
            }],
        })
        self.assertTrue(imminent["visible"])
        self.assertEqual(imminent["title"], "About to speak")
        self.assertEqual(imminent["tone"], "accent")
        self.assertEqual(imminent["detail"], "notes")
        speaking = self.osd.osd_view({**{
            "phase": "idle", "pane": "w1:p1", "speaking": True,
            "audible": True,
        }})
        self.assertEqual((speaking["title"], speaking["tone"]),
                         ("Speaking", "teal"))

    def test_dictation_outranks_speech_and_ready_uses_green(self):
        view = self.osd.osd_view({
            "phase": "recording",
            "speaking": True,
            "audible": True,
            "recording_seconds": 3,
        })
        self.assertTrue(view["title"].startswith("Listening"))
        self.assertEqual(view["tone"], "red")
        ready = self.osd.osd_view({
            "phase": "draft", "draft": True, "pane": "w1:p1",
        })
        self.assertEqual((ready["title"], ready["tone"]),
                         ("Ready to send", "green"))
        self.assertNotIn("secret", str(ready))

    def test_editor_changes_hide_ready_to_send_and_stale_reveal(self):
        for draft in (True, False):
            view = self.osd.osd_view({
                "phase": "draft" if draft else "idle", "draft": draft,
                "draft_edited": True, "osd": True,
            })
            self.assertFalse(view["visible"])
        pending = self.osd.osd_view({"draft_edited": True, "pending": True})
        self.assertEqual(pending["title"], "Ready to send")
        notice = self.osd.osd_view({
            "draft_edited": True, "osd": True,
            "osd_message": "Microphone unavailable",
        })
        self.assertTrue(notice["visible"])
        self.assertEqual(notice["title"], "Microphone unavailable")

    def test_attention_states_use_distinct_tones_without_reply_text(self):
        blocked = self.osd.osd_view({
            "phase": "idle", "pane": "w1:p1", "agent_state": "blocked",
            "reply": "secret reply",
        })
        self.assertEqual(blocked["title"], "Needs attention")
        self.assertEqual(blocked["tone"], "orange")
        self.assertNotIn("secret", str(blocked))
        retained = self.osd.osd_view({
            "retained": True,
            "retained_source": "notes",
            "reply": "secret reply",
        })
        self.assertEqual(retained["title"], "Dictation retained")
        self.assertEqual(retained["detail"], "From notes")


if __name__ == "__main__":
    unittest.main()
