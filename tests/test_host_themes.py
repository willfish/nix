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

    def test_terminal_roles_are_distinct(self):
        roles = (
            "base00",
            "base03",
            "base04",
            "base05",
            "base06",
            "base08",
            "base09",
            "base0A",
            "base0B",
            "base0C",
            "base0D",
            "base0E",
            "base0F",
        )
        for host, pair in self.catalogue.items():
            for mode in ("dark", "light"):
                with self.subTest(host=host, mode=mode):
                    colours = [pair[mode][role] for role in roles]
                    self.assertEqual(len(set(colours)), len(colours))

    def test_rendered_bright_and_thinking_colours_differ(self):
        expression = f"""
          let
            flake = builtins.getFlake {json.dumps(str(ROOT))};
            render = import {ROOT}/home/user/themes/render.nix {{
              lib = flake.inputs.nixpkgs.lib;
            }};
            palettes = import {ROOT}/home/user/themes/palettes.nix;
          in builtins.mapAttrs (host: pair: {{
            dark = {{
              ghostty = render.ghostty pair.dark;
              thinking = (render.pi "host" pair.dark).colors;
            }};
            light = {{
              ghostty = render.ghostty pair.light;
              thinking = (render.pi "host" pair.light).colors;
            }};
          }}) palettes
        """
        rendered = json.loads(
            subprocess.check_output(
                ["nix", "eval", "--impure", "--json", "--expr", expression],
                text=True,
            )
        )
        thinking_roles = (
            "thinkingOff",
            "thinkingMinimal",
            "thinkingLow",
            "thinkingMedium",
            "thinkingHigh",
            "thinkingXhigh",
            "thinkingMax",
        )
        for host, modes in rendered.items():
            for mode, payload in modes.items():
                with self.subTest(host=host, mode=mode):
                    slots = {}
                    for line in payload["ghostty"].splitlines():
                        if not line.startswith("palette = "):
                            continue
                        rest = line.split("=", 1)[1].strip()
                        index, colour = rest.split("=", 1)
                        slots[int(index)] = colour.lstrip("#")
                    self.assertEqual(set(slots), set(range(16)))
                    for index in range(8):
                        self.assertNotEqual(slots[index], slots[index + 8])
                    surfaces = [
                        self.catalogue[host][mode][key]
                        for key in ("base00", "base01", "base02")
                    ]
                    # Index 0 is the background, so it matches base00 by design.
                    for index, colour in slots.items():
                        if index == 0:
                            continue
                        for surface in surfaces:
                            self.assertGreaterEqual(
                                contrast(colour, surface),
                                4.5,
                                f"palette {index} #{colour} on #{surface}",
                            )
                    thinking = [
                        payload["thinking"][role].lstrip("#")
                        for role in thinking_roles
                    ]
                    self.assertEqual(len(set(thinking)), len(thinking))
                    self.assertEqual(
                        payload["thinking"]["dim"], payload["thinking"]["muted"]
                    )

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
