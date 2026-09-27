#!/usr/bin/env python3
"""Check one opaque sRGB pair, not accessibility or rendered composition."""

import argparse
import math
import re


def luminance(colour: str) -> float:
    """WCAG relative luminance for a six-digit opaque sRGB colour."""
    if not re.fullmatch(r"#[0-9a-fA-F]{6}", colour):
        raise ValueError("use an opaque six-digit sRGB colour such as #18212b")
    values = [int(colour[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
              for v in values]
    return sum(
        v * weight for v, weight in zip(linear, (0.2126, 0.7152, 0.0722)))


def ratio(first: str, second: str) -> float:
    light, dark = sorted((luminance(first), luminance(second)), reverse=True)
    return (light + 0.05) / (dark + 0.05)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("foreground")
    parser.add_argument("background")
    parser.add_argument("--minimum", type=float, default=4.5,
                        help="required ratio, 1 to 21 inclusive (default: 4.5)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.minimum) or not 1 <= args.minimum <= 21:
        parser.error("minimum must be finite and between 1 and 21")
    try:
        value = ratio(args.foreground, args.background)
    except ValueError as error:
        parser.error(str(error))
    passed = value >= args.minimum  # Never pass a rounded-up value.
    print(f"{'PASS' if passed else 'FAIL'} {value:.6f}:1; "
          f"minimum {args.minimum:g}:1 (opaque sRGB pair only)")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
