#!/usr/bin/env python3
"""Write ID3v2.4 tags into the copied library tree with id3v2.

Reads work/tagjobs.json (written by build-library.py) and shells out to id3v2,
which writes an id3v2.3-style tag: title, artist, album, artist-sorted, genre,
year, track number, TBPM and a TXXX comment with the archive track id.
Parallel because there are ~11k files; idempotent because the job list is
deterministic and id3v2 overwrites its own frames.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

BASE = os.environ.get("RADIO_ARCHIVE_BASE", "/srv/media/radio-archive")


def tag_one(job: dict, id3v2: str, apply: bool) -> tuple[str, str]:
    path = job["path"]
    if not os.path.exists(path):
        return ("missing", path)
    args = [id3v2, "-2"]  # id3v2 only: no lossy id3v1 tag
    args += ["-t", job.get("title") or ""]
    args += ["-a", job.get("artist") or ""]
    args += ["-A", job.get("album") or ""]
    if job.get("genre"):
        args += ["--TCON", job["genre"]]
    if job.get("year"):
        args += ["-y", job["year"]]
    if job.get("track"):
        args += ["-T", job["track"]]
    if job.get("bpm"):
        args += ["--TBPM", str(job["bpm"])]
    if job.get("artist"):
        args += ["--TPE2", job["artist"]]
    args += [
        "--TXXX",
        f"description:archive track id {job.get('track_id', '')}:eng",
    ]
    args.append(path)
    if not apply:
        return ("dry-run", path)
    try:
        res = subprocess.run(args, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return ("timeout", path)
    if res.returncode != 0:
        return (f"rc{res.returncode}:{res.stderr.strip()[:80]}", path)
    return ("ok", path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", default=f"{BASE}/work/tagjobs.json")
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    id3v2 = shutil.which("id3v2")
    if not id3v2:
        print(
            "id3v2 not in PATH; run inside nix shell nixpkgs#id3v2",
            file=sys.stderr,
        )
        return 3

    jobs = json.load(open(args.jobs))
    if args.limit:
        jobs = jobs[: args.limit]
    results = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for i, res in enumerate(
            pool.map(lambda j: tag_one(j, id3v2, args.apply), jobs), 1
        ):
            results.append(res)
            if i % 1000 == 0:
                print(f"  {i}/{len(jobs)}", flush=True)
    status = {}
    for st, _ in results:
        status[st] = status.get(st, 0) + 1
    print(
        f"tagged {len(jobs)} files "
        f"mode={'apply' if args.apply else 'dry-run'} {status}"
    )
    bad = [p for st, p in results if st not in ("ok", "dry-run")]
    with open(f"{BASE}/work/tag_failures.txt", "w") as fh:
        for p in bad:
            fh.write(p + "\n")
    print(f"failures: {len(bad)}")
    return 0 if not bad else 1


if __name__ == "__main__":
    raise SystemExit(main())
