"""Public-state projection and pill motion without a running compositor."""

import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1] / "home/config/voice"


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pill = load("voice_pill")
osd = load("voice_osd")


class PillTests(unittest.TestCase):
    def view(self, **status):
        return pill.pill_view(osd.osd_view(status))

    def test_idle_hidden_and_private_fields_never_projected(self):
        view = self.view(phase="idle", transcript="SECRET", reply="SECRET")
        self.assertFalse(view["visible"])
        self.assertNotIn("SECRET", str(view))

    def test_recording_reveals_destination_then_compacts(self):
        start = self.view(
            phase="recording",
            recording_seconds=1,
            recording_label="pi · Notes",
        )
        settled = self.view(
            phase="recording",
            recording_seconds=65,
            recording_label="pi · Notes",
        )
        self.assertTrue(start["expanded"])
        self.assertFalse(settled["expanded"])
        self.assertEqual(settled["timer"], "01:05")
        self.assertEqual(start["detail"], "Notes")
        self.assertLess(settled["width"], start["width"])

    def test_silence_remains_recording_not_an_error(self):
        view = self.view(
            phase="recording", recording_seconds=10, input_level=0
        )
        self.assertTrue(view["recording"])
        self.assertFalse(view["warning"])
        motion = pill.Motion()
        motion.step(view, 1)
        self.assertEqual(motion.levels, [0.0] * pill.BARS)

    def test_muted_and_clipping_expand_in_place(self):
        for flag, title in (
            ("muted", "Microphone muted"),
            ("clipping", "Input too loud"),
        ):
            with self.subTest(flag=flag):
                view = self.view(
                    phase="recording",
                    recording_seconds=10,
                    input_level=1,
                    microphone={flag: True},
                )
                self.assertTrue(view["expanded"])
                self.assertEqual(view["title"], title)
                self.assertEqual(view["tone"], "orange")
                if flag == "muted":
                    self.assertEqual(view["level"], 0)

    def test_work_speech_and_attention_never_show_input_activity(self):
        statuses = [
            {"phase": phase}
            for phase in ("starting", "stopping", "transcribing", "error")
        ] + [
            {"speaking": True, "audible": True},
            {"draft": True},
            {"retained": True},
            {"queued": True},
            {"rebind_needed": True},
            {"models": "unavailable"},
            {"connection_state": "reconnecting"},
            {"agent_state": "blocked"},
            {"osd": True, "osd_message": "Cancelled"},
        ]
        for status in statuses:
            with self.subTest(status=status):
                view = self.view(**status, input_level=1)
                self.assertTrue(view["visible"])
                self.assertFalse(view["recording"])
                self.assertEqual(view["level"], 0)
                compact = status.get("phase") in (
                    "stopping",
                    "transcribing",
                ) or status.get("speaking")
                self.assertEqual(view["expanded"], not compact)

    def test_active_capture_wins_over_speech_and_attention(self):
        view = self.view(
            phase="recording",
            recording_seconds=4,
            speaking=True,
            audible=True,
            agent_state="blocked",
        )
        self.assertTrue(view["recording"])
        self.assertEqual(view["tone"], "red")
        self.assertFalse(view["expanded"])

    def test_meter_clears_on_phase_change_and_hide(self):
        recording = self.view(
            phase="recording", recording_seconds=10, input_level=1
        )
        for next_view in (
            self.view(phase="transcribing"),
            self.view(phase="idle"),
        ):
            motion = pill.Motion()
            motion.step(recording, 1)
            self.assertEqual(motion.levels[-1], 1)
            motion.step(next_view, 1.1)
            self.assertEqual(motion.levels, [0] * pill.BARS)

    def test_animation_converges_and_stops_when_hidden(self):
        view = self.view(phase="recording", recording_seconds=10)
        motion = pill.Motion()
        for frame in range(120):
            self.assertTrue(motion.step(view, frame / 60))
        self.assertAlmostEqual(motion.width, 208, places=4)
        hidden = self.view(phase="idle")
        for frame in range(120, 240):
            active = motion.step(hidden, frame / 60)
        self.assertFalse(active)
        self.assertLess(motion.opacity, 0.01)

    def test_reduced_motion_snaps_to_target(self):
        view = self.view(phase="recording", recording_seconds=10)
        motion = pill.Motion()
        motion.step(view, 1, reduced=True)
        self.assertEqual(
            (motion.width, motion.height, motion.opacity), (208, 44, 1)
        )
        self.assertFalse(
            motion.step(self.view(phase="idle"), 2, reduced=True)
        )

    def test_nonfinite_input_and_elapsed_are_safe(self):
        for value in (float("nan"), float("inf"), "bad", None, -1):
            with self.subTest(value=value):
                view = self.view(
                    phase="recording",
                    recording_seconds=value,
                    input_level=value,
                )
                self.assertEqual(view["timer"], "00:00")
                self.assertGreaterEqual(view["level"], 0)
                self.assertLessEqual(view["level"], 1)

    def test_clipping_warning_outwaits_the_peak_latch(self):
        warnings = pill.WarningFilter()
        raw = osd.osd_view(
            {
                "phase": "recording",
                "recording_seconds": 8,
                "microphone": {"clipping": True},
            }
        )
        self.assertFalse(warnings.apply(raw, 0)["clipping"])
        self.assertFalse(warnings.apply(raw, 1.5)["clipping"])
        self.assertNotIn("clipping", warnings.apply(raw, 1.5)["detail"])
        self.assertTrue(warnings.apply(raw, 1.7)["clipping"])
        warnings.apply({**raw, "clipping": False}, 1.8)
        self.assertFalse(warnings.apply(raw, 2)["clipping"])

    def test_startup_grace_and_new_recordings_reset_clipping(self):
        warnings = pill.WarningFilter()
        raw = osd.osd_view(
            {
                "phase": "recording",
                "recording_seconds": 0,
                "microphone": {"clipping": True},
            }
        )
        self.assertFalse(warnings.apply(raw, 0)["clipping"])
        self.assertFalse(
            warnings.apply({**raw, "seconds": 1}, 1.8)["clipping"]
        )
        self.assertTrue(
            warnings.apply({**raw, "seconds": 2}, 2.1)["clipping"]
        )
        self.assertFalse(warnings.apply(raw, 3)["clipping"])
        warnings.apply(osd.osd_view({}), 4)
        self.assertFalse(
            warnings.apply({**raw, "seconds": 10}, 5)["clipping"]
        )

    def test_muting_is_immediate_even_during_clipping_grace(self):
        raw = osd.osd_view(
            {
                "phase": "recording",
                "recording_seconds": 0,
                "microphone": {"muted": True, "clipping": True},
            }
        )
        view = pill.pill_view(pill.WarningFilter().apply(raw, 0))
        self.assertEqual(view["title"], "Microphone muted")
        self.assertNotIn("clipping", view["detail"])

    def test_motion_never_overshoots_geometry(self):
        motion = pill.Motion()
        for frame in range(240):
            view = self.view(phase="recording", recording_seconds=frame % 10)
            motion.step(view, frame / 60)
            self.assertTrue(176 <= motion.width <= 352)
            self.assertTrue(36 <= motion.height <= 76)
            self.assertTrue(0 <= motion.opacity <= 1)


if __name__ == "__main__":
    unittest.main()
