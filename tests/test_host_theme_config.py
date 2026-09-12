"""Evaluate real Home Manager profiles, including headless and Darwin."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
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
                self.assertFalse(profile["tmuxFixed"])
                self.assertIn(
                    "--use-theme host-light/host-dark", profile["pi"]
                )
                self.assertIn('"$@"', profile["pi"])

    def test_headless_mac_omits_ghostty_but_preserves_linux_palettes(self):
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                if name == "william-darwin":
                    self.assertIsNone(profile["ghostty"])
                else:
                    self.assertIsInstance(profile["ghostty"], str)

    def test_cosmic_owns_only_graphical_linux(self):
        for name, profile in self.profiles.items():
            graphical = name not in ("william@terminus", "william-darwin")
            with self.subTest(profile=name):
                self.assertEqual(profile["cosmic"], graphical)
                self.assertTrue(profile["writableMode"])
                self.assertFalse(profile["stylixAutoEnable"])
                self.assertFalse(profile["gtkFixed"])
                self.assertEqual(profile["gtkEnable"], graphical)
                if not graphical:
                    self.assertEqual(profile["dconfSettings"], [])

    def test_picker_catalogue_defaults_and_activation(self):
        expected = {
            "william@andromeda": "rose-pine",
            "william@foundation": "tokyo-night",
            "william@starfish": "solarized",
        }
        for name, profile in self.profiles.items():
            with self.subTest(profile=name):
                if name not in expected:
                    self.assertIsNone(profile["catalogue"])
                    self.assertEqual(profile["reapply"], "")
                    continue
                catalogue = profile["catalogue"]
                self.assertEqual(catalogue["default"], expected[name])
                self.assertEqual(
                    set(catalogue["palettes"]),
                    {
                        "rose-pine",
                        "tokyo-night",
                        "solarized",
                        "catppuccin",
                        "gruvbox",
                    },
                )
                self.assertIn("theme-menu --reapply", profile["reapply"])
                self.assertIn(
                    "/theme-menu/active/host-light.json", profile["pi"]
                )
                for palette in catalogue["palettes"].values():
                    self.assertEqual(
                        set(palette["files"]),
                        {
                            "herdr.toml",
                            "host-palettes.json",
                            "host-light.json",
                            "host-dark.json",
                            "ghostty-light",
                            "ghostty-dark",
                        },
                    )
                    for mode in ("light", "dark"):
                        self.assertEqual(
                            palette["nvim"][mode]["base00"],
                            palette["herdrTheme"]["custom"][mode]["panel_bg"],
                        )

    def test_mode_migration_preserves_choice_and_remains_writable(self):
        profile = self.profiles["william@foundation"]
        for previous in (None, "true", "false"):
            with (
                self.subTest(previous=previous),
                tempfile.TemporaryDirectory() as d,
            ):
                directory = Path(d) / ".config/cosmic"
                mode = directory / "com.system76.CosmicTheme.Mode/v1/is_dark"
                mode.parent.mkdir(parents=True)
                if previous is not None:
                    old = Path(d) / "old-mode"
                    old.write_text(previous + "\n")
                    mode.symlink_to(old)
                # Home Manager removes the old managed mode symlink between
                # these two activation entries. A second activation must also
                # leave a subsequently selected light mode untouched.
                script = (
                    profile["rememberMode"] + '\nrm -f "$cosmicModePath"\n'
                )
                script += profile["writableModeScript"]
                env = {**os.environ, "HOME": d}
                subprocess.run(
                    ["bash", "-eu", "-c", script], env=env, check=True
                )
                self.assertFalse(mode.is_symlink())
                self.assertEqual(mode.read_text().strip(), previous or "true")
                mode.write_text("false\n")
                script = (
                    profile["rememberMode"] + profile["writableModeScript"]
                )
                subprocess.run(
                    ["bash", "-eu", "-c", script], env=env, check=True
                )
                self.assertEqual(mode.read_text().strip(), "false")
                self.assertEqual(
                    (mode.parent / "auto_switch").read_text().strip(), "false"
                )


if __name__ == "__main__":
    unittest.main()
