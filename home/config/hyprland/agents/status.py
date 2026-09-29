#!/usr/bin/env python3
"""Waybar JSON for the agents button. Reads usage records only."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import usage_lib


def usage_dir() -> Path:
    state = os.environ.get("XDG_STATE_HOME") or str(
        Path.home() / ".local/state"
    )
    return Path(state) / "omarchy/agents/usage"


def load_records() -> list[dict]:
    root = usage_dir()
    records = []
    if not root.is_dir():
        return records
    for path in sorted(root.glob("*.json")):
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(payload, dict):
            records.append(payload)
    return records


def main() -> int:
    json.dump(
        usage_lib.waybar_status(load_records()),
        sys.stdout,
        separators=(",", ":"),
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
