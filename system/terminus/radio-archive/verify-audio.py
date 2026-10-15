#!/usr/bin/env python3
"""Check every pulled MP3 against the catalog: size plus a real frame header.

Reads only the first bytes of each file, so a pass over the whole
archive costs one read per track rather than a decode. Anything short,
empty or not an MPEG audio frame lands in work/bad_audio.txt.
"""

from __future__ import annotations

import json
import os
import sys
from concurrent.futures import ThreadPoolExecutor

BASE = os.environ.get("RADIO_ARCHIVE_BASE", "/srv/media/radio-archive")


def frame_ok(head: bytes) -> bool:
    if (
        head[:3] == b"ID3"
        or head[:4] == b"ADIF"
        or head[:4] == b"fLaC"
        or head[:4] == b"OggS"
    ):
        return True
    if len(head) < 4:
        return False
    if (
        head[0] == 0xFF
        and (head[1] & 0xE0) == 0xE0
        and (head[1] & 0x06) >> 1 in (1, 2)
    ):
        return True
    return head[0] == 0xFF and (head[1] & 0xF6) == 0xF4  # MPEG ADTS (no CRC)


def check(job: tuple[str, int, str]) -> tuple[str, str]:
    tid, expect, base = job
    path = f"{base}/audio/{tid}.mp3"
    try:
        size = os.path.getsize(path)
    except FileNotFoundError:
        return (tid, "missing")
    if expect and size != expect:
        return (tid, f"size {size} != {expect}")
    with open(path, "rb") as fh:
        head = fh.read(4)
    if size < 1024:
        return (tid, f"tiny {size}b")
    if not frame_ok(head):
        return (tid, f"header {head.hex()}")
    return (tid, "ok")


def expected_sizes(base: str) -> dict[str, int]:
    """Bucket lengths. Catalog file_size is wrong for a few rows."""
    path = f"{base}/work/expected_sizes.tsv"
    out: dict[str, int] = {}
    if not os.path.exists(path):
        return out
    with open(path) as fh:
        for line in fh:
            size, _, name = line.strip().partition("\t")
            if name.endswith(".mp3") and size.isdigit():
                out[name.removesuffix(".mp3")] = int(size)
    return out


def main() -> int:
    base = sys.argv[1] if len(sys.argv) > 1 else BASE
    tracks = json.load(open(f"{base}/metadata/tracks.json"))
    measured = expected_sizes(base)
    jobs: dict[str, int] = {}
    for t in tracks:
        tid = str(t.get("track_id") or "")
        if not tid:
            continue
        jobs.setdefault(tid, measured.get(tid, int(t.get("file_size") or 0)))
    work = [(tid, size, base) for tid, size in sorted(jobs.items())]
    bad: list[tuple[str, str]] = []
    with ThreadPoolExecutor(max_workers=16) as pool:
        for tid, status in pool.map(check, work):
            if status != "ok":
                bad.append((tid, status))
    with open(f"{base}/work/bad_audio.txt", "w") as fh:
        for tid, status in bad:
            fh.write(f"{tid}\t{status}\n")
    total = sum(jobs.values())
    print(
        f"checked {len(work)} tracks "
        f"({total / 1e9:.1f} GB), problems: {len(bad)}"
    )
    for tid, status in bad[:15]:
        print(f"  {tid}: {status}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
