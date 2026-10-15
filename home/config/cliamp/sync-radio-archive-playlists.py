#!/usr/bin/env python3
"""Write one cliamp playlist per archive station.

cliamp expands a playlist address when that playlist is opened. It does
not expand a station address inside an already open list. Those rows are
played as live streams and reconnect forever.
"""

from __future__ import annotations

import os
import urllib.error
import urllib.request
from pathlib import Path

INDEX = os.environ.get(
    "RADIO_ARCHIVE_INDEX",
    "http://terminus:8085/playlists/index.m3u",
)
DEST = Path(
    os.environ.get(
        "RADIO_ARCHIVE_PLAYLISTS",
        Path.home() / ".config/cliamp/playlists",
    )
)
MARK = "radio-archive-generated"


def stations(text: str):
    title = None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXTINF:"):
            title = line.split(",", 1)[-1].strip() or "station"
        elif line.startswith("http") and title:
            yield title, line
            title = None


def playlist_text(title: str, url: str) -> str:
    safe = title.replace("\\", "\\\\").replace('"', '\\"')
    return f"# {MARK}\n[[track]]\npath = \"{url}\"\ntitle = \"{safe}\"\n"


def main() -> int:
    DEST.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(INDEX, timeout=8) as resp:
            body = resp.read().decode()
    except (OSError, urllib.error.URLError) as exc:
        print(f"radio archive index unavailable: {exc}")
        return 0

    seen = set()
    for title, url in stations(body):
        name = title.replace("/", "-").replace("\\", "-").strip()
        if not name or name in {".", ".."}:
            continue
        path = DEST / f"{name}.toml"
        if path.exists() and MARK not in path.read_text(errors="replace"):
            continue
        path.write_text(playlist_text(title, url))
        seen.add(path.name)

    for path in DEST.glob("*.toml"):
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        if MARK in text and path.name not in seen:
            path.unlink()

    old = DEST / "radio-archive.toml"
    drop = False
    if old.is_symlink():
        drop = True
    elif old.exists():
        body = old.read_text(errors="replace")
        drop = "playlists/index.m3u" in body
    if drop:
        old.unlink()
    print(f"wrote {len(seen)} archive playlists")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
