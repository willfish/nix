"""Parse every rendered ReGreet theme, not a hand-written TOML sample."""

import subprocess
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class GreeterThemeRenderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.out = Path(
            subprocess.check_output(
                [
                    "nix",
                    "build",
                    "--no-link",
                    "--print-out-paths",
                    "--file",
                    str(ROOT / "tests/greeter-theme-render.nix"),
                ],
                text=True,
            ).strip()
        )

    def test_all_catalogue_themes_parse(self):
        files = sorted(self.out.glob("*.toml"))
        self.assertEqual(len(files), 22)
        modes = set()
        for path in files:
            parsed = tomllib.loads(path.read_text())
            gtk = parsed["GTK"]
            dark = gtk["theme_name"] == "adw-gtk3-dark"
            self.assertEqual(gtk["application_prefer_dark_theme"], dark)
            self.assertIn(gtk["theme_name"], {"adw-gtk3", "adw-gtk3-dark"})
            self.assertEqual(gtk["font_name"], "Ubuntu 12")
            self.assertTrue(
                parsed["background"]["path"].startswith("/nix/store/")
            )
            self.assertEqual(parsed["background"]["fit"], "Cover")
            self.assertEqual(parsed["commands"]["reboot"][1], "reboot")
            self.assertEqual(parsed["commands"]["poweroff"][1], "poweroff")
            self.assertTrue(
                parsed["commands"]["reboot"][0].endswith("/bin/systemctl")
            )
            self.assertEqual(
                parsed["widget"]["clock"]["timezone"], "Europe/London"
            )
            modes.add(gtk["theme_name"])
        self.assertEqual(modes, {"adw-gtk3", "adw-gtk3-dark"})


if __name__ == "__main__":
    unittest.main()
