"""Render inputs for cosmic-settings to derive and validate the real theme."""

import json
from pathlib import Path
import sys


def rgb(colour):
    return tuple(int(colour[i : i + 2], 16) / 255 for i in (0, 2, 4))


def colour(channels, alpha=False):
    fields = [
        f"{key}: {value:.8f}"
        for key, value in zip(("red", "green", "blue"), channels)
    ]
    if alpha:
        fields.append("alpha: 1.0")
    return "(" + ", ".join(fields) + ")"


def builder(p, mode, name):
    def c(key, alpha=False):
        return colour(rgb(p[key]), alpha)

    roles = {
        "bright_red": "base08",
        "bright_green": "base0B",
        "bright_orange": "base09",
        "gray_1": "base01",
        "gray_2": "base02",
        "accent_blue": "base0D",
        "accent_indigo": "base0D",
        "accent_purple": "base0E",
        "accent_pink": "base0F",
        "accent_red": "base08",
        "accent_orange": "base09",
        "accent_yellow": "base0A",
        "accent_green": "base0B",
        "accent_warm_grey": "base04",
        "ext_warm_grey": "base04",
        "ext_orange": "base09",
        "ext_yellow": "base0A",
        "ext_blue": "base0D",
        "ext_purple": "base0E",
        "ext_pink": "base0F",
        "ext_indigo": "base0D",
    }
    palette = [f"name: {json.dumps(name)}"]
    palette += [f"{key}: {c(role, True)}" for key, role in roles.items()]
    # COSMIC's neutral scale runs from the background towards the foreground in
    # both modes. Keep light controls and panels warm too.
    bg, fg = rgb(p["base00"]), rgb(p["base05"])
    for i in range(11):
        channels = tuple(a + (b - a) * i / 10 for a, b in zip(bg, fg))
        palette.append(f"neutral_{i}: {colour(channels, True)}")

    fields = [f"palette: {mode.capitalize()}((" + ",".join(palette) + "))"]
    spaces = {
        "none": 0,
        "xxxs": 4,
        "xxs": 8,
        "xs": 12,
        "s": 16,
        "m": 24,
        "l": 32,
        "xl": 48,
        "xxl": 64,
        "xxxl": 128,
    }
    fields.append(
        "spacing: ("
        + ",".join(f"space_{k}: {v}" for k, v in spaces.items())
        + ")"
    )
    radii = {"0": 0, "xs": 4, "s": 8, "m": 16, "l": 32, "xl": 160}
    fields.append(
        "corner_radii: ("
        + ",".join(
            f"radius_{k}: ({v}.0,{v}.0,{v}.0,{v}.0)" for k, v in radii.items()
        )
        + ")"
    )
    fields += ["neutral_tint: None", "text_tint: None"]
    for field, role, alpha in (
        ("bg_color", "base00", True),
        ("primary_container_bg", "base01", True),
        ("secondary_container_bg", "base02", True),
        ("accent", "base0D", False),
        ("success", "base0B", False),
        ("warning", "base0A", False),
        ("destructive", "base08", False),
        ("window_hint", "base0D", False),
    ):
        fields.append(f"{field}: Some({c(role, alpha)})")
    fields += ["is_frosted: false", "gaps: (0, 8)", "active_hint: 3"]
    return "(\n" + ",\n".join(fields) + ",\n)\n"


if __name__ == "__main__":
    pair = json.loads(Path(sys.argv[1]).read_text())
    output = Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    for mode in ("dark", "light"):
        (output / f"{mode}.ron").write_text(
            builder(pair[mode], mode, pair["herdr"][f"{mode}_name"])
        )
