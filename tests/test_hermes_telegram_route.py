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

HOUSE = {"group_id": "-1001", "topics": {"Qwen": 787, "General": 1}}


class TelegramRouteTests(unittest.TestCase):
    def test_adds_topic_route_once(self):
        config, changed = merge_routes({"model": "grok-4.6"}, house=HOUSE)
        self.assertTrue(changed)
        route = config["profile_routes"][0]
        self.assertEqual(route["profile"], "qwen")
        self.assertEqual(route["chat_id"], "-1001")
        self.assertEqual(route["thread_id"], "787")
        again, changed = merge_routes(config, house=HOUSE)
        self.assertFalse(changed)
        self.assertIs(again, config)

    def test_replaces_catch_all_telegram_route(self):
        config = {
            "multiplex_profiles": True,
            "gateway": {"multiplex_profiles": True},
            "profile_routes": [
                {
                    "name": "telegram-qwen",
                    "platform": "telegram",
                    "profile": "qwen",
                    "enabled": True,
                }
            ],
        }
        config, changed = merge_routes(config, house=HOUSE)
        self.assertTrue(changed)
        self.assertEqual(config["profile_routes"][0]["thread_id"], "787")

    def test_without_qwen_topic_does_not_capture_all_telegram(self):
        house = {"group_id": "-1"}
        config, changed = merge_routes({"model": "grok"}, house=house)
        self.assertFalse(changed)
        self.assertFalse(config.get("profile_routes"))

    def test_file_merge_is_quiet_and_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yaml"
            path.write_text("model: fixture\n")
            captured = io.StringIO()
            with redirect_stdout(captured):
                self.assertTrue(merge_into_config_file(path, house=HOUSE))
                self.assertFalse(merge_into_config_file(path, house=HOUSE))
            self.assertEqual(captured.getvalue(), "")
            text = path.read_text()
            self.assertIn("model: fixture", text)
            self.assertIn("thread_id: '787'", text)


if __name__ == "__main__":
    unittest.main()
