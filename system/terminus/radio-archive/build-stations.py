#!/usr/bin/env python3
"""Turn the radio archive catalog dump into local radio stations.

The app drives each channel from `audio_channels.playlist_data`, which holds one
entry per energy tier in one of two shapes:

  {"low": [{"track_id": "86973", "weight": 1.0}, ...]}   explicit
  {"low": {"k": 100, "energy": "LOW", "tracks": [...]}}   matrix

Both are honoured. Weighted pools are rotated by sampling in proportion to the
weight the service itself assigned, seeded so the same playlist comes back on
every run. Channels also get one playlist per tier plus a combined playlist that
round-robins the tiers, which is what the player does while you work.

Anything the official playlists never reference becomes a clearly-labelled
`genre` station from genre_category, plus `energy` stations for the
low/medium/high tiers and one station containing the whole library.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import random
import re
import unicodedata
import urllib.parse

BASE = os.environ.get("RADIO_ARCHIVE_BASE", "/srv/media/radio-archive")
TIERS = ("low", "medium", "high")
# MagicDNS short name. tailscale0 is trusted, so this resolves off the LAN and
# on it. Do not use terminus.local: that name only exists on the home network.
STREAM_BASE = "http://terminus:8085"
# cliamp's resolveM3U rejects a plain playlist body over 1 MiB.
CLIAMP_M3U_LIMIT = 1_000_000


def slug(text: str, fallback: str, limit: int = 70) -> str:
    text = unicodedata.normalize("NFKD", text or "")
    text = re.sub(r"[^\w\s.-]", "", text, flags=re.UNICODE)
    text = re.sub(r"\s+", "-", text.strip()).strip("-.").lower()
    return text or fallback


def load_index(path: str) -> dict[str, str]:
    out: dict[str, str] = {}
    with open(path) as fh:
        next(fh, None)
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                out[parts[0]] = parts[2]
    return out


def tier_entries(value) -> list[tuple[str, float]]:
    """Normalise either playlist_data shape to (track_id, weight) pairs."""
    if isinstance(value, dict):
        value = value.get("tracks") or []
    out: list[tuple[str, float]] = []
    for entry in value or []:
        if isinstance(entry, dict):
            tid = str(entry.get("track_id") or "").strip()
            try:
                weight = float(entry.get("weight") or 1.0)
            except (TypeError, ValueError):
                weight = 1.0
        else:
            tid = str(entry or "").strip()
            weight = 1.0
        if tid:
            out.append((tid, max(weight, 0.01)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--seed", type=int, default=20261014)
    ap.add_argument("--min-hours", type=float, default=6.0)
    ap.add_argument("--max-entries", type=int, default=2500)
    ap.add_argument(
        "--stream-base",
        default=STREAM_BASE,
        help="base URL for playlist entries and station stream URLs",
    )
    ap.add_argument(
        "--local",
        action="store_true",
        help="write filesystem paths instead of HTTP URLs into the playlists",
    )
    args = ap.parse_args()

    tracks = json.load(open(f"{args.base}/metadata/tracks.json"))
    channels = json.load(open(f"{args.base}/metadata/channels.json"))
    index = load_index(f"{args.base}/work/library_index.tsv")

    by_id = {str(t.get("track_id")): t for t in tracks if t.get("track_id")}
    usable = {tid: by_id[tid] for tid in index if tid in by_id}
    unresolved: list[str] = []
    claimed: set[str] = set()
    stations: list[dict] = []

    emitted_ids: set[str] = set()
    base_url = args.stream_base.rstrip("/")

    def entry_for(tid: str) -> str:
        # audio/<id>.mp3 is stable. music/ paths change with titles.
        if args.local:
            return index[tid]
        return f"{base_url}/audio/{urllib.parse.quote(tid)}.mp3"

    def playlist_bytes(ids: list[str]) -> int:
        size = len("#EXTM3U\n".encode())
        for tid in ids:
            t = usable[tid]
            dur = int(t.get("duration_seconds") or 0)
            title = (t.get("track_name") or f"track {tid}").strip()
            artist = (t.get("artist_name") or "Unknown").strip()
            line = f"#EXTINF:{dur},{artist} - {title}\n{entry_for(tid)}\n"
            size += len(line.encode())
        return size

    def fit(ids: list[str]) -> list[str]:
        """Keep a prefix cliamp can expand. Over 1 MiB is refused."""
        if playlist_bytes(ids) <= CLIAMP_M3U_LIMIT:
            return ids
        lo, hi, best = 0, len(ids), 0
        while lo <= hi:
            mid = (lo + hi) // 2
            if playlist_bytes(ids[:mid]) <= CLIAMP_M3U_LIMIT:
                best = mid
                lo = mid + 1
            else:
                hi = mid - 1
        return ids[:best]

    def emit(path: str, ids: list[str]) -> tuple[int, int]:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        emitted_ids.update(ids)
        secs = 0
        with open(path, "w") as fh:
            fh.write("#EXTM3U\n")
            for tid in ids:
                t = usable[tid]
                dur = int(t.get("duration_seconds") or 0)
                secs += dur
                title = (t.get("track_name") or f"track {tid}").strip()
                artist = (t.get("artist_name") or "Unknown").strip()
                fh.write(f"#EXTINF:{dur},{artist} - {title}\n")
                fh.write(entry_for(tid) + "\n")
        return len(ids), secs

    def rotate(pairs: list[tuple[str, float]], seed_extra: int) -> list[str]:
        """Weighted sample, long enough to run for --min-hours."""
        pairs = [(t, w) for t, w in pairs if t in usable]
        if not pairs:
            return []
        rng = random.Random(args.seed + seed_extra)
        avg = sum(
            int(usable[t].get("duration_seconds") or 600) for t, _ in pairs
        ) / len(pairs)
        need = max(len(pairs), int(args.min_hours * 3600 / max(avg, 60)))
        need = min(need, args.max_entries)
        weights = [w for _, w in pairs]
        ids = [
            t
            for t in rng.choices([t for t, _ in pairs], weights=weights, k=need)
        ]
        return ids

    def plain_shuffle(ids: list[str], seed_extra: int) -> list[str]:
        ids = list(dict.fromkeys(i for i in ids if i in usable))
        if not ids:
            return []
        rng = random.Random(args.seed + seed_extra)
        avg = sum(
            int(usable[i].get("duration_seconds") or 600) for i in ids
        ) / len(ids)
        cycles = max(
            1, min(6, int(args.min_hours * 3600 / max(avg, 60) // len(ids)) + 1)
        )
        out: list[str] = []
        for _ in range(cycles):
            shuffled = ids[:]
            rng.shuffle(shuffled)
            out.extend(shuffled)
        return out

    def register(
        kind: str,
        name: str,
        path: str,
        ids: list[str],
        extra: dict | None = None,
    ) -> dict:
        full = len(ids)
        ids = fit(ids)
        if len(ids) != full:
            print(
                f"trimmed {name}: {full} -> {len(ids)} "
                f"to stay under {CLIAMP_M3U_LIMIT} bytes"
            )
        count, secs = emit(path, ids)
        rel = os.path.relpath(path, args.base)
        station = {
            "name": name,
            "kind": kind,
            "playlist": path,
            "tracks": count,
            "unique_tracks": len(set(ids)),
            "hours": round(secs / 3600, 2),
            "stream_url": f"{base_url}/{urllib.parse.quote(rel, safe='/')}",
        }
        station.update(extra or {})
        stations.append(station)
        return station

    # ---- official channels -------------------------------------------------
    for chan in sorted(channels, key=lambda c: c["channel_number"]):
        num = chan["channel_number"]
        name = str(chan.get("channel_name") or f"channel {num}").strip()
        base_slug = f"{num:02d}-{slug(name, f'channel{num}')}"
        pd = chan.get("playlist_data") or {}
        tiers: dict[str, list[tuple[str, float]]] = {}
        for tier in TIERS:
            pairs = tier_entries(pd.get(tier))
            good = []
            for tid, weight in pairs:
                if tid in usable:
                    good.append((tid, weight))
                else:
                    unresolved.append(f"{num}\t{name}\t{tier}\t{tid}")
            if good:
                tiers[tier] = good
        if not tiers:
            register(
                "official-empty",
                f"{num:02d} {name}",
                f"{args.base}/playlists/official/{base_slug}.m3u",
                [],
                {"description": (chan.get("description") or "").strip()},
            )
            continue

        combined: list[str] = []
        per_tier = {
            t: rotate(p, num * 10 + i) for i, (t, p) in enumerate(tiers.items())
        }
        longest = max(len(v) for v in per_tier.values())
        for i in range(longest):
            for tier in TIERS:
                seq = per_tier.get(tier) or []
                if seq:
                    combined.append(seq[i % len(seq)])

        art = f"{args.base}/artwork/{base_slug}.jpg"
        extra = {
            "channel_number": num,
            "collection": chan.get("collection"),
            "intensity": chan.get("intensity"),
            "description": (chan.get("description") or "").strip(),
            "artwork": art if os.path.exists(art) else None,
            "tiers": {t: len(set(v)) for t, v in per_tier.items()},
        }
        register(
            "official",
            f"{num:02d} {name}",
            f"{args.base}/playlists/official/{base_slug}.m3u",
            combined,
            extra,
        )
        for tier, seq in per_tier.items():
            register(
                "official-tier",
                f"{num:02d} {name} ({tier})",
                f"{args.base}/playlists/official/{base_slug}--{tier}.m3u",
                seq,
                {"channel_number": num, "tier": tier},
            )
        for seq in per_tier.values():
            claimed.update(seq)

    # ---- genre stations for the rest of the pool ---------------------------
    leftover = collections.defaultdict(list)
    for tid, t in usable.items():
        if tid in claimed:
            continue
        genre = (
            t.get("genre_category") or ""
        ).strip() or f"Untagged ({t.get('energy_level') or 'no tier'})"
        leftover[genre].append(tid)
    for i, (genre, ids) in enumerate(
        sorted(leftover.items(), key=lambda kv: -len(kv[1]))
    ):
        seq = plain_shuffle(ids, 500 + i)
        register(
            "genre",
            genre,
            f"{args.base}/playlists/genre/{slug(genre, 'genre')}.m3u",
            seq,
        )

    # ---- energy tiers and the whole library --------------------------------
    tiers_all = collections.defaultdict(list)
    for tid, t in usable.items():
        tiers_all[t.get("energy_level") or "unlabelled"].append(tid)
    for i, (tier, ids) in enumerate(sorted(tiers_all.items())):
        register(
            "energy",
            tier,
            f"{args.base}/playlists/energy/{slug(tier, 'tier')}.m3u",
            plain_shuffle(ids, 900 + i),
        )

    all_ids = plain_shuffle(sorted(usable), 1)
    everything = register(
        "all",
        "Everything",
        f"{args.base}/playlists/radio-archive-all.m3u",
        all_ids,
    )
    stations.remove(everything)
    stations.insert(0, everything)

    index_kinds = {"all", "official", "energy", "genre"}
    with open(f"{args.base}/playlists/index.m3u", "w") as fh:
        fh.write("#EXTM3U\n")
        for station in stations:
            if station["kind"] not in index_kinds or station["tracks"] == 0:
                continue
            fh.write(f"#EXTINF:-1,{station['name']}\n")
            fh.write(station["stream_url"] + "\n")

    with open(f"{args.base}/metadata/stations.json", "w") as fh:
        json.dump(stations, fh, indent=1)
    with open(f"{args.base}/metadata/stations.csv", "w") as fh:
        fh.write(
            "kind\tname\tunique_tracks\tplaylist_tracks\thours\tplaylist\n"
        )
        for s in stations:
            fh.write(
                "\t".join(
                    (
                        s["kind"],
                        s["name"],
                        str(s["unique_tracks"]),
                        str(s["tracks"]),
                        str(s["hours"]),
                        s["playlist"],
                    )
                )
                + "\n"
            )
    with open(f"{args.base}/metadata/unresolved_official_ids.tsv", "w") as fh:
        fh.write("channel\tname\ttier\ttrack_id\n")
        for line in dict.fromkeys(unresolved):
            fh.write(line + "\n")

    kinds = collections.Counter(s["kind"] for s in stations)
    used = emitted_ids
    print(f"stations written: {len(stations)}  {dict(kinds)}")
    print(
        f"library tracks used: {len(used)} / {len(usable)} "
        f"unresolved official ids: {len(set(unresolved))}"
    )
    for s in stations[:1]:
        print(
            f"  {s['kind']:9} {s['name'][:34]:36} "
            f"{s['unique_tracks']:6} uniq {s['tracks']:6} "
            f"entries {s['hours']} h"
        )
    for s in stations[1:6]:
        print(
            f"  {s['kind']:9} {s['name'][:34]:36} "
            f"{s['unique_tracks']:6} uniq {s['tracks']:6} "
            f"entries {s['hours']} h"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
