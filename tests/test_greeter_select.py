"""Keep greeter selection bounded, nonblocking and within the allowlist."""

import importlib.util
import os
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "greeter_select",
    ROOT / "system/modules/greeter_select.py",
)
select = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(select)


class GreeterSelectTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.themes = {
            "tokyo-night": {
                "config": (
                    "/nix/store/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-tokyo.toml"
                ),
                "css": "/nix/store/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-tokyo.css",
            },
            "rose-pine": {
                "config": (
                    "/nix/store/cccccccccccccccccccccccccccccccc-rose.toml"
                ),
                "css": "/nix/store/dddddddddddddddddddddddddddddddd-rose.css",
            },
        }

    def write(self, text):
        path = self.directory / "william"
        path.write_text(text)
        return path

    def test_reads_id_with_optional_newline(self):
        self.write("tokyo-night\n")
        self.assertEqual(select.read_bounded(self.directory), "tokyo-night")

    def test_missing_empty_and_rejected_ids_are_absent(self):
        self.assertIsNone(select.read_bounded(self.directory))
        self.write("")
        self.assertIsNone(select.read_bounded(self.directory))
        self.write("Tokyo-Night\n")
        self.assertIsNone(select.read_bounded(self.directory))
        self.write("tokyo-night\nextra\n")
        self.assertIsNone(select.read_bounded(self.directory))
        self.write("../rose-pine\n")
        self.assertIsNone(select.read_bounded(self.directory))
        self.write("a" * 65)
        self.assertIsNone(select.read_bounded(self.directory))

    def test_symlink_file_and_parent_are_not_followed(self):
        target = self.directory / "elsewhere"
        target.write_text("rose-pine\n")
        os.symlink(target, self.directory / "william")
        self.assertIsNone(select.read_bounded(self.directory))
        linked = self.directory / "linked"
        os.symlink(self.directory, linked)
        self.write("tokyo-night\n")
        self.assertIsNone(select.read_bounded(linked))

    def test_fifo_does_not_block(self):
        os.mkfifo(self.directory / "william")
        started = time.monotonic()
        holder = {}

        def read():
            holder["value"] = select.read_bounded(self.directory)

        thread = threading.Thread(target=read)
        thread.start()
        thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertIsNone(holder["value"])
        self.assertLess(time.monotonic() - started, 2)

    def test_non_regular_file_is_rejected(self):
        os.mkdir(self.directory / "william")
        self.assertIsNone(select.read_bounded(self.directory))

    def test_unknown_id_uses_allowlisted_fallback_only(self):
        chosen = select.resolve("not-a-theme", self.themes, "rose-pine")
        self.assertEqual(chosen["config"], self.themes["rose-pine"]["config"])
        with self.assertRaises(SystemExit):
            select.resolve(
                "tokyo-night",
                {
                    "tokyo-night": {
                        "config": "/home/william/theme.toml",
                        "css": self.themes["tokyo-night"]["css"],
                    }
                },
                "tokyo-night",
            )

    def test_command_uses_store_assets_for_the_selected_id(self):
        manifest = {
            "fallback": "rose-pine",
            "sessionShare": "/nix/store/session/share",
            "dbus": "/nix/store/dbus/bin/dbus-run-session",
            "cage": "/nix/store/cage/bin/cage",
            "cageArgs": ["-s", "-d"],
            "regreet": "/nix/store/regreet/bin/regreet",
            "themes": self.themes,
        }
        argv, env = select.command_for(manifest, "tokyo-night")
        self.assertEqual(
            argv[argv.index("--config") + 1],
            self.themes["tokyo-night"]["config"],
        )
        self.assertEqual(
            argv[argv.index("--style") + 1], self.themes["tokyo-night"]["css"]
        )
        self.assertTrue(
            env["XDG_DATA_DIRS"].startswith("/nix/store/session/share:")
        )


if __name__ == "__main__":
    unittest.main()
