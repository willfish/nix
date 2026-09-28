#!/usr/bin/env python3
"""Point Omapager's icon helper at installed chat and GitHub icons."""

import sys
from pathlib import Path


def replace_once(text, old, new, label):
    if old not in text:
        raise SystemExit(f"missing {label}")
    return text.replace(old, new, 1)


def main():
    path = Path(sys.argv[1])
    text = path.read_text()
    text = replace_once(
        text,
        "def from_icon_theme(names):\n"
        '    """Whatever the machine already has for this name."""\n'
        "    for name in names:",
        "def from_icon_theme(names):\n"
        '    """Whatever the machine already has for this name."""\n'
        "    names = icon_paths.expand_names(names)\n"
        "    for name in names:",
        "icon theme lookup",
    )
    text = replace_once(
        text,
        "if want in haystack.split(\"-\") or "
        '("-" + want + "-") in ("-" + haystack + "-"):',
        "if icon_paths.name_matches(want, haystack):",
        "desktop name match",
    )
    path.write_text(text)


if __name__ == "__main__":
    main()
