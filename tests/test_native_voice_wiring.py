"""Bind offline voice tests to the selected immutable native package."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "check_behavior", ROOT / "scripts/check-behavior.py"
)
behavior = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(behavior)


class NativeVoiceWiringTests(unittest.TestCase):
    def test_fixture_comes_from_candidate_not_the_active_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            launcher = Path(directory) / "pi-voice"
            launcher.write_text(
                '#!/bin/sh\nexport PI_VOICE_COMMAND="$0"\n'
                'exec /nix/store/candidate-pi-voice-native/bin/'
                'pi-voice-c "$@"\n'
            )
            self.assertEqual(
                behavior.native_voice_fixture(launcher),
                Path(
                    "/nix/store/candidate-pi-voice-native/libexec/"
                    "voice-controller-fixture"
                ),
            )

    def test_legacy_launcher_cannot_silently_test_a_different_controller(self):
        with tempfile.TemporaryDirectory() as directory:
            launcher = Path(directory) / "pi-voice"
            launcher.write_text('exec python3 /old/voice_controller.py "$@"\n')
            with self.assertRaisesRegex(ValueError, "not native"):
                behavior.native_voice_fixture(launcher)

    def test_profiles_without_voice_have_no_candidate_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIsNone(
                behavior.native_voice_fixture(Path(directory) / "absent")
            )

    def test_only_an_exec_line_selects_the_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            launcher = Path(directory) / "pi-voice"
            launcher.write_text(
                '# exec /nix/store/old/bin/pi-voice-c "$@"\n'
                'echo "exec /nix/store/old/bin/pi-voice-c"\n'
                'exec /nix/store/current/bin/pi-voice-c "$@"\n'
            )
            self.assertEqual(
                behavior.native_voice_fixture(launcher),
                Path("/nix/store/current/libexec/voice-controller-fixture"),
            )


if __name__ == "__main__":
    unittest.main()
