"""Shared palettes: readable surfaces, soft light and stable host identity."""

import json
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


def luminance(colour):
    channels = [int(colour[i : i + 2], 16) / 255 for i in (0, 2, 4)]
    linear = [
        c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
        for c in channels
    ]
    return sum(
        c * weight for c, weight in zip(linear, (0.2126, 0.7152, 0.0722))
    )


def contrast(a, b):
    values = sorted((luminance(a), luminance(b)))
    return (values[1] + 0.05) / (values[0] + 0.05)


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

    def test_host_identities(self):
        expected = {
            "andromeda": "rose-pine",
            "foundation": "tokyo-night",
            "starfish": "solarized",
            "terminus": "catppuccin",
            "relay": "gruvbox",
        }
        self.assertEqual(
            {
                host: p["herdr"]["dark_name"]
                for host, p in self.catalogue.items()
            },
            expected,
        )

    def test_readable_text_on_every_surface(self):
        for host, pair in self.catalogue.items():
            for mode in ("dark", "light"):
                p = pair[mode]
                for fg in (
                    "base04",
                    "base05",
                    *(f"base{i:02X}" for i in range(8, 16)),
                ):
                    for bg in ("base00", "base01", "base02"):
                        with self.subTest(host=host, mode=mode, fg=fg, bg=bg):
                            self.assertGreaterEqual(contrast(p[fg], p[bg]), 4.5)

    def test_soft_warm_light_surfaces(self):
        for host, pair in self.catalogue.items():
            for key in ("base00", "base01", "base02"):
                p = pair["light"][key]
                with self.subTest(host=host, surface=key):
                    self.assertLess(luminance(p), 0.88)
                    self.assertGreater(int(p[:2], 16), int(p[4:], 16))
                    self.assertNotEqual(p, "ffffff")

    def test_distinct_host_accents_and_complete_base16(self):
        for mode in ("dark", "light"):
            accents = [p[mode]["base0D"] for p in self.catalogue.values()]
            self.assertEqual(len(set(accents)), len(accents))
            for p in self.catalogue.values():
                self.assertEqual(
                    set(p[mode]), {f"base{i:02X}" for i in range(16)}
                )


if __name__ == "__main__":
    unittest.main()
