"""Declared Pi settings fill missing keys and leave existing values alone."""
import json
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "home/config/pi/merge-settings.py"
DEFAULTS = ROOT / "home/config/pi/settings-defaults.json"
KEYBINDINGS = ROOT / "home/config/pi/keybindings-defaults.json"


class PiSettingsDefaultsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.settings = Path(self.tmp.name) / "agent" / "settings.json"

    def run_merge(self, defaults=None):
        defaults_path = Path(self.tmp.name) / "defaults.json"
        payload = defaults if defaults is not None else json.loads(
            DEFAULTS.read_text()
        )
        defaults_path.write_text(json.dumps(payload) + "\n")
        subprocess.run(
            ["python3", str(SCRIPT), str(defaults_path), str(self.settings)],
            check=True,
        )
        return json.loads(self.settings.read_text())

    def test_declared_defaults_include_quiet_startup(self):
        defaults = json.loads(DEFAULTS.read_text())
        self.assertEqual(defaults["defaultProvider"], "xai")
        self.assertEqual(defaults["defaultModel"], "grok-4.7")
        self.assertEqual(defaults["quietStartup"], True)
        self.assertEqual(defaults["editorPaddingX"], 1)
        keybindings = json.loads(KEYBINDINGS.read_text())
        self.assertEqual(keybindings["app.session.rename"], "ctrl+shift+r")

    def test_creates_settings_when_missing(self):
        merged = self.run_merge()
        self.assertEqual(merged["defaultProvider"], "xai")
        self.assertEqual(merged["defaultModel"], "grok-4.7")
        self.assertEqual(merged["quietStartup"], True)
        self.assertEqual(merged["editorPaddingX"], 1)
        mode = stat.S_IMODE(self.settings.stat().st_mode)
        self.assertEqual(mode, 0o600)

    def test_migrates_grok_4_6_default_to_grok_4_7(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(
            json.dumps({
                "defaultProvider": "xai",
                "defaultModel": "grok-4.6",
            }) + "\n"
        )
        merged = self.run_merge()
        self.assertEqual(merged["defaultModel"], "grok-4.7")
        self.assertEqual(merged["defaultProvider"], "xai")

    def test_fills_only_missing_keys(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(
            json.dumps({"theme": "dark", "editorPaddingX": 2}) + "\n"
        )
        merged = self.run_merge()
        self.assertEqual(merged["theme"], "dark")
        self.assertEqual(merged["editorPaddingX"], 2)
        self.assertEqual(merged["quietStartup"], True)

    def test_does_not_rewrite_when_complete(self):
        first = self.run_merge()
        stamp = self.settings.stat().st_mtime_ns
        second = self.run_merge()
        self.assertEqual(first, second)
        self.assertEqual(self.settings.stat().st_mtime_ns, stamp)

    def test_rejects_non_object_settings(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text("[]\n")
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_merge()
