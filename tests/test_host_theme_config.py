"""Evaluate real Home Manager profiles, including headless and Darwin."""

import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HostThemeConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profiles = json.loads(
            subprocess.check_output(
                [
                    "nix",
                    "eval",
                    "--impure",
                    "--json",
                    "--file",
                    str(ROOT / "tests/host-theme-config.nix"),
                ],
                text=True,
            )
        )

    def test_same_palette_across_layers(self):
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                for mode in ("light", "dark"):
                    p = profile["nvim"][mode]
                    self.assertEqual(
                        profile["theme"]["custom"][mode]["panel_bg"],
                        p["base00"],
                    )
                    self.assertEqual(
                        profile["theme"]["custom"][mode]["accent"], p["base0D"]
                    )
                self.assertEqual(
                    profile["stylix"],
                    profile["nvim"]["dark"]["base00"].lstrip("#"),
                )
                self.assertTrue(profile["theme"]["auto_switch"])
                self.assertFalse(profile["fishFixed"])
                self.assertIn(
                    "--use-theme host-light/host-dark", profile["pi"]
                )
                self.assertIn('"$@"', profile["pi"])

    def test_delta_follows_terminal_palette(self):
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                delta = profile["delta"]
                self.assertTrue(delta["navigate"])
                self.assertEqual(delta["features"], "line-numbers decorations")
                self.assertNotIn("light", delta)
                self.assertNotIn("theme", delta)
                self.assertEqual(delta["plus-style"], "syntax green")
                self.assertEqual(delta["minus-style"], "syntax red")

    def test_headless_mac_omits_ghostty_but_preserves_linux_palettes(self):
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                if name in ("william-darwin", "william@relay"):
                    self.assertIsNone(profile["ghostty"])
                else:
                    self.assertIsInstance(profile["ghostty"], str)

    def test_menu_owns_only_graphical_linux(self):
        for name, profile in self.profiles.items():
            graphical = name not in (
                "william@terminus",
                "william@relay",
                "william-darwin",
            )
            with self.subTest(profile=name):
                self.assertEqual(profile["graphical"], graphical)
                self.assertFalse(profile["stylixAutoEnable"])
                self.assertFalse(profile["gtkFixed"])
                self.assertEqual(profile["gtkEnable"], graphical)
                if not graphical:
                    self.assertEqual(profile["dconfSettings"], [])

    def test_picker_catalogue_defaults_and_activation(self):
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                self.assertTrue(profile["themeMatchesHost"])
                self.assertEqual(
                    profile["voiceMenu"], {"width": 55, "lines": 10}
                )
                voice = profile["voiceFuzzel"]
                if voice:
                    self.assertNotIn("[colors]", voice)
                    self.assertNotIn("[border]", voice)
                    self.assertNotIn("font=", voice)
                if not profile["graphical"]:
                    self.assertIsNone(profile["catalogue"])
                    self.assertEqual(profile["reapply"], "")
                    self.assertIsNone(profile["fuzzelIni"])
                    continue
                catalogue = profile["catalogue"]
                self.assertEqual(catalogue["default"], profile["expectedTheme"])
                self.assertEqual(
                    set(catalogue["palettes"]), set(profile["paletteNames"])
                )
                self.assertIn("theme-menu --reapply", profile["reapply"])
                self.assertIn(
                    "/theme-menu/active/host-light.json", profile["pi"]
                )
                active = profile["activeFuzzel"]
                self.assertIn(f"include={active}", profile["fuzzelIni"])
                self.assertEqual(
                    profile["fuzzelIni"], profile["hyprlandFuzzel"]
                )
                self.assertTrue(profile["sameDefaultFuzzel"])
                voice = profile["voiceFuzzel"]
                # Voice menu is installed only where speech-to-text is enabled.
                if name in ("william@andromeda", "william@foundation"):
                    self.assertIsNotNone(voice)
                if voice:
                    self.assertIn(f"include={active}", voice)
                    self.assertNotIn("[colors]", voice)
                    self.assertNotIn("[border]", voice)
                    self.assertNotIn("font=", voice)
                    self.assertIn("width=55", voice)
                    self.assertIn("lines=10", voice)
                for palette in catalogue["palettes"].values():
                    self.assertIn(palette["nativeMode"], ("light", "dark"))
                    self.assertEqual(
                        set(palette["files"]),
                        {
                            "herdr.toml",
                            "host-palettes.json",
                            "host-light.json",
                            "host-dark.json",
                            "ghostty-light",
                            "ghostty-dark",
                            "btop.theme",
                        },
                    )
                    for mode in ("light", "dark"):
                        self.assertEqual(
                            palette["nvim"][mode]["base00"],
                            palette["herdrTheme"]["custom"][mode]["panel_bg"],
                        )


if __name__ == "__main__":
    unittest.main()
