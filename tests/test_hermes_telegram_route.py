"""Telegram routing merge is idempotent and does not log config."""

import io
import unittest
from contextlib import redirect_stdout
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1] / "home/config/local-llm"
sys.path.insert(0, str(ROOT))
from hermes_telegram_route import (  # noqa: E402
    merge_into_config_file,
    merge_routes,
)


class TelegramRouteTests(unittest.TestCase):
    def test_adds_multiplex_route_once(self):
        config, changed = merge_routes({"model": "grok-4.6"})
        self.assertTrue(changed)
        self.assertTrue(config["multiplex_profiles"])
        self.assertTrue(config["gateway"]["multiplex_profiles"])
        self.assertEqual(config["profile_routes"][0]["profile"], "qwen")
        self.assertEqual(config["profile_routes"][0]["platform"], "telegram")
        again, changed = merge_routes(config)
        self.assertFalse(changed)
        self.assertIs(again, config)

    def test_file_merge_is_quiet_and_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yaml"
            path.write_text("model: fixture\n")
            captured = io.StringIO()
            with redirect_stdout(captured):
                self.assertTrue(merge_into_config_file(path))
                self.assertFalse(merge_into_config_file(path))
            self.assertEqual(captured.getvalue(), "")
            text = path.read_text()
            self.assertIn("model: fixture", text)
            self.assertIn("telegram-qwen", text)


if __name__ == "__main__":
    unittest.main()
