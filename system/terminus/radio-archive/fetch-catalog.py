#!/usr/bin/env python3
"""Refresh catalog metadata for the pull and the playlists.

Writes metadata/tracks.json, metadata/channels.json and work/urls.txt,
then fills work/expected_sizes.tsv from the bucket. Use --sizes for the
size pass: one 1-byte range request per object. That length is the only
trustworthy completion number, because catalog rows disagree with the
stored bytes.

The REST key is the public anon key shipped in the web player bundle.
Row-level security decides what it can read. Override it with:

  RADIO_ARCHIVE_SUPABASE_KEY=... python3 fetch-catalog.py --sizes
"""

from __future__ import annotations

import argparse
import json
import os
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor

PROJECT = "xewajlyswijmjxuajhif"
REST = f"https://{PROJECT}.supabase.co/rest/v1"
BUCKET = f"https://{PROJECT}.supabase.co/storage/v1/object/public/audio-files"
ANON_KEY = os.environ.get(
    "RADIO_ARCHIVE_SUPABASE_KEY",
    (
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inhld2FqbHlzd2lqbWp4dWFqaGlm"
        "Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjMxNjk2NjQsImV4cCI6MjA3ODc0"
        "NTY2NH0.p8SoMqXOJjz-Sa41df4yxW6LSFDZp-YNRplPN9buTfU"
    ),
)
BASE = os.environ.get("RADIO_ARCHIVE_BASE", "/srv/media/radio-archive")


def get(url: str, headers: dict[str, str] | None = None) -> bytes:
    req = urllib.request.Request(url, headers=headers or {})
    # Fixed https host. S310 is a URL-open warning.
    with urllib.request.urlopen(req, timeout=90) as resp:  # noqa: S310
        return resp.read()


def query(table: str, select: str = "*", pages: int = 60) -> list[dict]:
    headers = {"apikey": ANON_KEY, "Authorization": f"Bearer {ANON_KEY}"}
    rows: list[dict] = []
    for page in range(pages):
        url = (
            f"{REST}/{table}?{select}&order=id.asc"
            f"&offset={page * 1000}&limit=1000"
        )
        batch = json.loads(get(url, headers))
        rows.extend(batch)
        if len(batch) < 1000:
            break
    return rows


def flat(row: dict) -> dict:
    outer = row.get("metadata") or {}
    inner = outer.get("metadata") or {}
    track_id = str(outer.get("track_id") or inner.get("track_id") or "")
    # file_size is the name verify-audio.py and build-library.py already read.
    # It is the catalog claim, not the bucket length; expected_sizes.tsv wins.
    catalog_size = int(
        float(
            outer.get("file_size")
            or outer.get("file_size_bytes")
            or inner.get("file_length")
            or 0
        )
    )
    return {
        "track_id": track_id,
        "row_id": row.get("id"),
        "file_path": row.get("file_path") or f"{track_id}.mp3",
        "duration_seconds": row.get("duration_seconds")
        or inner.get("duration")
        or 0,
        "energy_level": row.get("energy_level") or "",
        "catalog_size": catalog_size,
        "file_size": catalog_size,
        "track_name": inner.get("track_name") or "",
        "artist_name": inner.get("artist_name") or "",
        "album_name": inner.get("album_name") or "",
        "genre_category": inner.get("genre_category") or "",
        "intensity": inner.get("intensity") or "",
        "bpm": inner.get("tempo") or outer.get("bpm") or "",
        "created_at": inner.get("created_at") or "",
        "legacy_channel_ids": str(inner.get("channel_ids") or ""),
        "legacy_channels": str(inner.get("channels") or ""),
    }


def object_size(name: str) -> tuple[str, int]:
    """1-byte range request. Content-Range has the length."""
    try:
        with urllib.request.urlopen(  # noqa: S310
            urllib.request.Request(
                f"{BUCKET}/{urllib.parse.quote(name)}",
                headers={"Range": "bytes=0-0"},
            ),
            timeout=60,
        ) as resp:
            total = resp.headers.get("Content-Range", "").rpartition("/")[2]
            return (name, int(total))
    except Exception:  # noqa: BLE001 - unreachable is a row
        return (name, -1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=BASE)
    ap.add_argument(
        "--sizes",
        action="store_true",
        help="ask the bucket for every object length",
    )
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()

    os.makedirs(f"{args.base}/metadata", exist_ok=True)
    os.makedirs(f"{args.base}/work", exist_ok=True)

    tracks = [flat(r) for r in query("audio_tracks") if r.get("id")]
    channels = json.loads(
        get(
            f"{REST}/audio_channels?*&limit=200",
            {"apikey": ANON_KEY, "Authorization": f"Bearer {ANON_KEY}"},
        )
    )
    json.dump(tracks, open(f"{args.base}/metadata/tracks.json", "w"))
    json.dump(channels, open(f"{args.base}/metadata/channels.json", "w"))

    names = sorted({t["file_path"] for t in tracks if t["track_id"]})
    with open(f"{args.base}/work/urls.txt", "w") as fh:
        for name in names:
            fh.write(f"{BUCKET}/{urllib.parse.quote(name)}\n")
    print(
        f"catalog: {len(tracks)} rows, {len(names)} objects, "
        f"{len(channels)} channels"
    )

    if args.sizes:
        with (
            open(f"{args.base}/work/expected_sizes.tsv", "w") as fh,
            ThreadPoolExecutor(max_workers=args.workers) as pool,
        ):
            unknown = 0
            for i, (name, size) in enumerate(pool.map(object_size, names), 1):
                if size < 0:
                    unknown += 1
                    continue
                fh.write(f"{size}\t{name}\n")
                if i % 2000 == 0:
                    print(f"  {i}/{len(names)}", flush=True)
        print(f"sizes: {len(names) - unknown} measured, {unknown} unreadable")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
