"""Advance the active theme through its packaged backgrounds.

The theme package keeps every image under backgrounds/. The session shows one
PNG. A hand-set override is a single file and does not rotate.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def next_name(names, current, preferred):
    ordered = [
        name
        for name in names
        if isinstance(name, str)
        and "/" not in name
        and name not in (".", "..")
    ]
    if len(ordered) < 2:
        return None
    if current not in ordered:
        current = preferred if preferred in ordered else ordered[0]
    return ordered[(ordered.index(current) + 1) % len(ordered)]


def wallpaper_reference(catalogue, selection, mode):
    key = (
        catalogue.get("default")
        if selection in ("", "default")
        else selection
    )
    palette = (catalogue.get("palettes") or {}).get(key) or {}
    session = ((palette.get("session") or {}).get(mode) or {})
    source = session.get("wallpaper.png")
    if not isinstance(source, str) or not source.startswith("nix-theme:"):
        return None
    name = source.removeprefix("nix-theme:")
    if not name or any(part in name for part in ("/", "..", " ")):
        return None
    return name


def _read_json(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError):
        return None


def advance(state, catalogue_path, flake, magick="magick", nix="nix"):
    """Show the next background. Return True when the image changed."""
    state = Path(state)
    link = state / "wallpaper-source"
    current_path = state / "wallpaper-current"
    live = state / "active" / "wallpaper-live.png"
    catalogue = _read_json(catalogue_path)
    if catalogue is None:
        return False
    try:
        selection = (state / "selection").read_text().strip()
    except OSError:
        selection = "default"
    try:
        mode = (state / "mode").read_text().strip()
    except OSError:
        mode = "dark"
    if mode not in ("light", "dark"):
        mode = "dark"
    theme_id = wallpaper_reference(catalogue, selection, mode)
    if theme_id is None:
        live.unlink(missing_ok=True)
        current_path.unlink(missing_ok=True)
        if link.is_symlink() or link.exists():
            link.unlink()
        return False
    if not link.exists():
        subprocess.run(
            [
                nix,
                "build",
                "--out-link",
                str(link),
                f"{flake}#theme-{theme_id}",
            ],
            check=True,
        )
    anchored = os.readlink(link) if link.is_symlink() else None
    package = link.resolve()
    meta = _read_json(package / "theme.json") or {}
    names = meta.get("backgrounds") or []
    current = ""
    if current_path.exists():
        current = current_path.read_text().strip()
    chosen = next_name(names, current, meta.get("preferred"))
    if chosen is None:
        return False
    source = (package / "backgrounds" / chosen).resolve()
    background_dir = (package / "backgrounds").resolve()
    if source.parent != background_dir or not source.is_file():
        return False
    live.parent.mkdir(parents=True, exist_ok=True)
    temporary = live.with_name(".wallpaper-live.png.tmp")
    subprocess.run([magick, str(source), f"PNG:{temporary}"], check=True)
    if link.is_symlink() and os.readlink(link) != anchored:
        temporary.unlink(missing_ok=True)
        return False
    if current_path.exists() and current_path.read_text().strip() != current:
        temporary.unlink(missing_ok=True)
        return False
    os.replace(temporary, live)
    current_path.write_text(chosen + "\n")
    os.chmod(current_path, 0o600)
    return True


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", required=True)
    parser.add_argument("--catalogue", required=True)
    parser.add_argument("--flake", required=True)
    parser.add_argument("--magick", default="magick")
    args = parser.parse_args(argv)
    changed = advance(args.state, args.catalogue, args.flake, args.magick)
    return 0 if changed else 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
