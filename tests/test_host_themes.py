"""Native Omarchy palettes keep upstream colours and stable theme IDs."""

import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HostThemesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalogue = json.loads(
            subprocess.check_output(
                [
                    "nix",
                    "eval",
                    "--json",
                    "--file",
                    str(ROOT / "home/user/themes/palettes.nix"),
                ],
                text=True,
            )
        )

    def test_theme_ids_are_native_omarchy_names(self):
        self.assertIn("rose-pine", self.catalogue)
        self.assertIn("osaka-jade", self.catalogue)
        self.assertNotIn("solarized", self.catalogue)
        self.assertNotIn("andromeda", self.catalogue)
        for name, theme in self.catalogue.items():
            with self.subTest(theme=name):
                self.assertEqual(theme["herdr"]["name"], name)
                self.assertEqual(theme["herdr"]["dark_name"], name)
                self.assertEqual(theme["herdr"]["light_name"], name)
                self.assertIn(theme["nativeMode"], ("light", "dark"))
                # Both slots hold the native palette, not synthetic variants.
                self.assertEqual(theme["light"], theme["dark"])

    def test_distinct_accents_and_complete_base16(self):
        for mode in ("dark", "light"):
            accents = [p[mode]["base0D"] for p in self.catalogue.values()]
            self.assertEqual(len(set(accents)), len(accents))
            for theme in self.catalogue.values():
                self.assertEqual(
                    set(theme[mode]), {f"base{i:02X}" for i in range(16)}
                )


if __name__ == "__main__":
    unittest.main()
