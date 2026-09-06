"""Regression tests for the Home Manager Hermes profile overlay."""

import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest

import yaml


SOURCE = (
    Path(__file__).resolve().parents[1]
    / "home/config/local-llm/hermes_profile.py"
)


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SOURCE.exists(), "Profile overlay helper is missing")
        spec = importlib.util.spec_from_file_location("hermes_profile", SOURCE)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_preserves_unrelated_settings_and_provider_secrets(self):
        original = {
            "display": {"theme": "custom", "streaming": False},
            "custom_providers": [
                {"name": "another", "api_key": "test-secret"},
                {"name": "qwen-local", "base_url": "http://localhost:8080"},
            ],
        }
        overlay = {
            "display": {"streaming": True},
            "custom_providers": [
                {
                    "name": "qwen-local",
                    "base_url": "http://127.0.0.1:8081/v1",
                }
            ],
        }
        merged = self.module.merge_profile(original, overlay)
        self.assertEqual(
            merged["display"], {"theme": "custom", "streaming": True}
        )
        self.assertEqual(len(merged["custom_providers"]), 2)
        self.assertEqual(
            merged["custom_providers"][0]["api_key"], "test-secret"
        )
        self.assertEqual(
            merged["custom_providers"][1]["base_url"],
            "http://127.0.0.1:8081/v1",
        )
        self.assertFalse(original["display"]["streaming"])

    def test_backup_private_permissions_and_idempotence(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "config.yaml"
            overlay = Path(directory) / "overlay.json"
            original = (
                "agent:\n  max_turns: 90\ncompression:\n  enabled: false\n"
            )
            profile.write_text(original)
            overlay.write_text(json.dumps({
                "agent": {"max_turns": "unlimited"},
                "compression": {"enabled": True},
            }))
            self.assertTrue(self.module.apply_profile(profile, overlay))
            pattern = "config.yaml.before-local-llm-*"
            backups = list(Path(directory).glob(pattern))
            self.assertEqual(len(backups), 1)
            self.assertEqual(backups[0].read_text(), original)
            self.assertEqual(stat.S_IMODE(profile.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(backups[0].stat().st_mode), 0o600)
            applied = yaml.safe_load(profile.read_text())
            self.assertEqual(applied["agent"]["max_turns"], "unlimited")
            self.assertFalse(self.module.apply_profile(profile, overlay))
            self.assertEqual(len(list(Path(directory).glob(pattern))), 1)

    def test_rejects_symlink_profile_without_changing_target(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "target.yaml"
            target.write_text("display: {}\n")
            profile = Path(directory) / "config.yaml"
            profile.symlink_to(target)
            overlay = Path(directory) / "overlay.json"
            overlay.write_text("{}")
            with self.assertRaises(ValueError):
                self.module.apply_profile(profile, overlay)
            self.assertEqual(target.read_text(), "display: {}\n")

    def test_creates_a_missing_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "qwen/config.yaml"
            overlay = Path(directory) / "overlay.json"
            overlay.write_text('{"agent":{"max_turns":"unlimited"}}')
            self.assertTrue(self.module.apply_profile(profile, overlay))
            applied = yaml.safe_load(profile.read_text())
            self.assertEqual(applied["agent"]["max_turns"], "unlimited")

    def test_runtime_key_supports_direct_hermes_without_entering_overlay(self):
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "config.yaml"
            overlay = Path(directory) / "overlay.json"
            key_file = Path(directory) / "api-key"
            key_file.write_text("fixture-only-private-key\n")
            overlay.write_text(json.dumps({
                "model": {"provider": "qwen-local"},
                "custom_providers": [{"name": "qwen-local"}],
            }))
            self.module.apply_profile(profile, overlay, key_file=key_file)
            applied = yaml.safe_load(profile.read_text())
            self.assertEqual(
                applied["custom_providers"][0]["api_key"],
                "fixture-only-private-key",
            )
            self.assertEqual(
                applied["model"]["api_key"], "fixture-only-private-key"
            )
            self.assertNotIn("fixture-only-private-key", overlay.read_text())
            self.assertEqual(stat.S_IMODE(profile.stat().st_mode), 0o600)


if __name__ == "__main__":
    unittest.main()
