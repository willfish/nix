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
DEFAULT_MODEL_KEY = "opencode-go/space-bunny-free"


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
        self.assertEqual(defaults["defaultProvider"], "opencode-go")
        self.assertEqual(defaults["defaultModel"], "space-bunny-free")
        self.assertEqual(
            defaults["modelThinkingLevels"][DEFAULT_MODEL_KEY], "high"
        )
        self.assertEqual(defaults["quietStartup"], True)
        self.assertEqual(defaults["editorPaddingX"], 1)
        self.assertEqual(defaults["extensions"], ["-builtin:mcp"])
        keybindings = json.loads(KEYBINDINGS.read_text())
        self.assertEqual(keybindings["app.session.rename"], "ctrl+shift+r")

    def test_creates_settings_when_missing(self):
        merged = self.run_merge()
        self.assertEqual(merged["defaultProvider"], "opencode-go")
        self.assertEqual(merged["defaultModel"], "space-bunny-free")
        self.assertEqual(
            merged["modelThinkingLevels"][DEFAULT_MODEL_KEY], "high"
        )
        self.assertEqual(merged["quietStartup"], True)
        self.assertEqual(merged["editorPaddingX"], 1)
        self.assertEqual(merged["extensions"], ["-builtin:mcp"])
        mode = stat.S_IMODE(self.settings.stat().st_mode)
        self.assertEqual(mode, 0o600)

    def test_migrates_leftover_defaults_to_declared_default(self):
        for provider, model in (
            ("xai", "grok-4.6"),
            ("xai", "grok-4.7"),
            ("opencode-go", "glm-5.3"),
        ):
            with self.subTest(model=model):
                self.settings.parent.mkdir(parents=True, exist_ok=True)
                self.settings.write_text(
                    json.dumps({
                        "defaultProvider": provider,
                        "defaultModel": model,
                        "theme": "dark",
                    }) + "\n"
                )
                merged = self.run_merge()
                self.assertEqual(merged["defaultProvider"], "opencode-go")
                self.assertEqual(merged["defaultModel"], "space-bunny-free")
                self.assertEqual(merged["theme"], "dark")

    def test_leaves_custom_default_model_alone(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(
            json.dumps({
                "defaultProvider": "xai",
                "defaultModel": "grok-4.5",
            }) + "\n"
        )
        merged = self.run_merge()
        self.assertEqual(merged["defaultProvider"], "xai")
        self.assertEqual(merged["defaultModel"], "grok-4.5")

    def test_fills_only_missing_keys(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(
            json.dumps({"theme": "dark", "editorPaddingX": 2}) + "\n"
        )
        merged = self.run_merge()
        self.assertEqual(merged["theme"], "dark")
        self.assertEqual(merged["editorPaddingX"], 2)
        self.assertEqual(merged["quietStartup"], True)
        self.assertEqual(merged["extensions"], ["-builtin:mcp"])
        self.assertEqual(
            merged["modelThinkingLevels"][DEFAULT_MODEL_KEY], "high"
        )

    def test_leaves_existing_extension_overrides_alone(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(
            json.dumps({"extensions": ["-builtin:codemode"]}) + "\n"
        )
        merged = self.run_merge()
        self.assertEqual(merged["extensions"], ["-builtin:codemode"])

    def test_does_not_rewrite_when_complete(self):
        first = self.run_merge()
        stamp = self.settings.stat().st_mtime_ns
        second = self.run_merge()
        self.assertEqual(first, second)
        self.assertEqual(self.settings.stat().st_mtime_ns, stamp)

    def test_flips_previous_om_default_once(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text(
            json.dumps({
                "observational-memory-jev": {"enabledByDefault": True},
            }) + "\n"
        )
        merged = self.run_merge()
        self.assertFalse(
            merged["observational-memory-jev"]["enabledByDefault"]
        )
        self.settings.write_text(
            json.dumps({
                "observational-memory-jev": {"enabledByDefault": True},
            }) + "\n"
        )
        again = self.run_merge()
        self.assertTrue(again["observational-memory-jev"]["enabledByDefault"])

    def test_rejects_non_object_settings(self):
        self.settings.parent.mkdir(parents=True)
        self.settings.write_text("[]\n")
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_merge()
