#!/usr/bin/env python3
"""Waybar JSON for the arXiv scanner badge."""

import json
import os
from pathlib import Path


def load(path: Path) -> dict:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def as_list(value: object) -> list:
    return value if isinstance(value, list) else []


def as_text(value: object) -> str:
    return value if isinstance(value, str) else ""


def main() -> None:
    home = Path(os.environ.get("HOME", ""))
    state_home = Path(
        os.environ.get("XDG_STATE_HOME", home / ".local/state")
    )
    config_home = Path(
        os.environ.get("XDG_CONFIG_HOME", home / ".config")
    )
    state = load(state_home / "omarchy-arxiv-scanner/state.json")
    viewed = load(state_home / "omarchy-arxiv-scanner/last_viewed.json")
    config = load(config_home / "omarchy-arxiv-scanner/config.json")

    areas = as_list(state.get("area_matches"))
    watched = as_list(state.get("watched_matches"))
    total = len(areas) + len(watched)
    updated = as_text(state.get("updated_at"))
    seen = as_text(viewed.get("viewed_at"))
    unseen = bool(updated) and updated != seen
    category = as_text(config.get("category")) or "arXiv"
    interests = as_list(config.get("interestAreas"))
    authors = as_list(config.get("watchedAuthors"))

    tooltip = f"{category}: {total} match(es)"
    if unseen:
        tooltip += " · not opened since the last scan"
    if not interests and not authors:
        tooltip += " · set interests in the panel"
    tooltip += " · right-click scans now"
    text = str(total) if total else "\uf0c3"
    if total and unseen:
        text = "!" + text

    print(
        json.dumps(
            {
                "text": text,
                "tooltip": tooltip,
                "class": "unseen" if unseen else "idle",
            }
        )
    )


if __name__ == "__main__":
    main()
