#!/usr/bin/env python3
"""Build the local radio archive library layout from the pulled metadata.

audio/<track_id>.mp3            pristine copies as pulled from the bucket
music/<Artist>/<Album>/NN - <Title>.mp3    tagged, copy, app-browsable tree
unsorted/<energy>/<track_id>.mp3           pulls that carry no embedded tags

Writes metadata/library_index.tsv (track_id, source, library path, status) and
work/tagjobs.tsv for the tag pass. Idempotent: existing library files with the
expected size are left alone.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys

BASE = os.environ.get("RADIO_ARCHIVE_BASE", "/srv/media/radio-archive")


def slug(text: str, fallback: str, limit: int = 90) -> str:
    text = (text or "").strip()
    text = re.sub(r"[^\w\s.&'()-]", "", text, flags=re.UNICODE)
    text = re.sub(r"\s+", " ", text).strip(" ._")
    if not text:
        return fallback
    return text[:limit]


def year_of(track: dict) -> str:
    for key in ("created_at", "released"):
        value = str(track.get(key) or "")
        match = re.search(r"(19|20)\d{2}", value)
        if match:
            return match.group(0)
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tracks", default=f"{BASE}/metadata/tracks.json")
    ap.add_argument("--audio", default=f"{BASE}/audio")
    ap.add_argument("--music", default=f"{BASE}/music")
    ap.add_argument("--unsorted", default=f"{BASE}/unsorted")
    ap.add_argument("--work", default=f"{BASE}/work")
    ap.add_argument(
        "--apply", action="store_true", help="create files (default: dry run)"
    )
    args = ap.parse_args()

    tracks = json.load(open(args.tracks))
    by_id: dict[str, dict] = {}
    for track in tracks:
        tid = str(track.get("track_id") or "").strip()
        src = os.path.join(args.audio, f"{tid}.mp3")
        if not tid or not os.path.exists(src):
            continue
        # Prefer the largest tagged record if a track id repeats.
        prev = by_id.get(tid)
        if (
            prev
            and (prev.get("track_name") or "")
            and not (track.get("track_name") or "")
        ):
            continue
        by_id[tid] = track

    index_rows: list[list[str]] = []
    tag_jobs: list[dict] = []
    seen_paths: dict[str, str] = {}

    ordered = sorted(
        by_id.items(),
        key=lambda kv: (
            kv[1].get("artist_name") or "zzz",
            kv[1].get("album_name") or "",
            kv[1].get("duration_seconds") or 0,
            kv[0],
        ),
    )
    album_seq: dict[str, int] = {}

    for tid, track in ordered:
        src = os.path.join(args.audio, f"{tid}.mp3")
        title = (track.get("track_name") or "").strip()
        artist = (track.get("artist_name") or "").strip()
        album = (track.get("album_name") or "").strip()
        genre = (track.get("genre_category") or "").strip()
        size = int(track.get("file_size") or 0)

        if not title:
            tier = track.get("energy_level") or "unsorted"
            dst = os.path.join(
                args.unsorted, slug(tier, "unsorted"), f"{tid}.mp3"
            )
            status = "unsorted"
        else:
            artist_dir = slug(artist, "Various Artists")
            album_dir = slug(album, f"{artist_dir} - singles")
            key = f"{artist_dir}/{album_dir}"
            album_seq[key] = album_seq.get(key, 0) + 1
            num = album_seq[key]
            name = f"{num:02d} - {slug(title, tid)}.mp3"
            dst = os.path.join(args.music, artist_dir, album_dir, name)
            if dst in seen_paths:
                dst = os.path.join(
                    args.music,
                    artist_dir,
                    album_dir,
                    f"{num:02d} - {slug(title, tid)} ({tid}).mp3",
                )
            seen_paths[dst] = tid
            status = "library"
            tag_jobs.append(
                {
                    "path": dst,
                    "title": title,
                    "artist": artist or "Various Artists",
                    "album": album or title,
                    "genre": genre,
                    "year": year_of(track),
                    "bpm": str(track.get("bpm") or ""),
                    "track": str(num),
                    "track_id": tid,
                }
            )

        if os.path.exists(dst) and (not size or os.path.getsize(dst) == size):
            action = "keep"
        elif args.apply:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.exists(dst):
                os.remove(dst)
            shutil.copy2(src, dst)
            action = "copy"
        else:
            action = "would-copy"

        index_rows.append(
            [tid, src, dst, status, action, genre, artist, album, title]
        )

    os.makedirs(args.work, exist_ok=True)
    with open(f"{args.work}/library_index.tsv", "w") as fh:
        fh.write(
            "track_id\tsource\tlibrary_path\tstatus\taction\t"
            "genre\tartist\talbum\ttitle\n"
        )
        for row in index_rows:
            fh.write("\t".join(row) + "\n")
    with open(f"{args.work}/tagjobs.json", "w") as fh:
        json.dump(tag_jobs, fh)

    lib = sum(1 for r in index_rows if r[3] == "library")
    uns = sum(1 for r in index_rows if r[3] == "unsorted")
    missing = len(by_id) - len(index_rows)
    albums = len({(r[6], r[7]) for r in index_rows if r[3] == "library"})
    artists = len({r[6] for r in index_rows if r[3] == "library"})
    print(
        f"tracks with local file: {len(index_rows)} "
        f"library: {lib} unsorted: {uns}"
        f"artists: {artists} albums: {albums} "
        f"mode: {'apply' if args.apply else 'dry-run'}"
    )
    if not by_id:
        print(
            "no audio files found yet; run after the audio pull",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
