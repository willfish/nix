#!/usr/bin/env python3
"""Fill missing keys in a writable Pi settings.json from declared defaults."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def merge_missing(defaults: dict, settings: dict) -> tuple[dict, bool]:
    merged = dict(settings)
    changed = False
    for key, value in defaults.items():
        if key not in merged:
            merged[key] = value
            changed = True
    return merged, changed


def apply(defaults_path: Path, settings_path: Path) -> bool:
    defaults = json.loads(defaults_path.read_text())
    if not isinstance(defaults, dict):
        raise ValueError("defaults must be a JSON object")
    if settings_path.exists():
        settings = json.loads(settings_path.read_text() or "{}")
        if not isinstance(settings, dict):
            raise ValueError("settings must be a JSON object")
    else:
        settings = {}
    merged, changed = merge_missing(defaults, settings)
    if not changed:
        return False
    settings_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = settings_path.with_name(settings_path.name + ".tmp")
    temporary.write_text(json.dumps(merged, indent=2) + "\n")
    temporary.chmod(0o600)
    os.replace(temporary, settings_path)
    return True


def main() -> int:
    if len(sys.argv) != 3:
        print(
            f"usage: {sys.argv[0]} <defaults.json> <settings.json>",
            file=sys.stderr,
        )
        return 2
    apply(Path(sys.argv[1]), Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
