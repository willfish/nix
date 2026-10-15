#!/usr/bin/env python3
"""Print station names and URLs from metadata/stations.json.

This is for inspection. Do not commit the block: cliamp has no remote station
source, and the names belong on the archive, not in radios.toml.
"""

from __future__ import annotations

import argparse
import os
import json
import sys

BASE = os.environ.get("RADIO_ARCHIVE_BASE", "/srv/media/radio-archive")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stations", default=f"{BASE}/metadata/stations.json")
    ap.add_argument("--min-genre-tracks", type=int, default=200)
    args = ap.parse_args()

    stations = json.load(open(args.stations))
    entries: list[tuple[str, str]] = []
    for station in stations:
        kind = station["kind"]
        if kind == "genre" and station["unique_tracks"] < args.min_genre_tracks:
            continue
        if kind not in {"all", "official", "energy", "genre"}:
            continue
        name = station["name"]
        entries.append((name, station["stream_url"]))

    for name, url in sorted(entries, key=lambda e: e[0].lower()):
        print("[[station]]")
        print(f'name = "{name}"')
        print(f'url = "{url}"')
        print()
    print(f"# {len(entries)} radio archive stations", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
