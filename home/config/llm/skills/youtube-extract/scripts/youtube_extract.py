#!/usr/bin/env python3
"""Extract YouTube metadata and captions without downloading media."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


VIDEO_ID_RE = re.compile(
    r"(?:youtu\.be/|youtube\.com/(?:watch\?(?:.*&)?v=|embed/|live/|shorts/|v/))"
    r"([A-Za-z0-9_-]{11})"
)
TIME_QUERY_RE = re.compile(r"[?&#]t=([0-9hmsHMS]+)")
CLOCK_RE = re.compile(
    r"^(?:(\d{1,2}):)?(\d{1,2}):(\d{2})(?:[.,](\d{1,3}))?"
)
TAG_RE = re.compile(r"</?[^>]+>")
VTT_HEADER_RE = re.compile(
    r"^(WEBVTT|NOTE|STYLE|Kind:|Language:|REGION)\b", re.I
)


@dataclass(frozen=True)
class Cue:
    start: float
    end: float
    text: str


@dataclass(frozen=True)
class VideoRef:
    url: str
    video_id: str | None
    timestamp: float | None


def parse_clock(value: str) -> float:
    match = CLOCK_RE.fullmatch(value.strip())
    if not match:
        raise ValueError(f"invalid timestamp: {value!r}")
    hours, minutes, seconds, fraction = match.groups()
    millis = (fraction or "0").ljust(3, "0")[:3]
    return (
        int(hours or 0) * 3600
        + int(minutes) * 60
        + int(seconds)
        + int(millis) / 1000
    )


def parse_offset(value: str | None) -> float | None:
    if value is None or value == "":
        return None
    token = value.strip()
    if re.fullmatch(r"\d+(?:\.\d+)?", token):
        return float(token)
    match = re.fullmatch(
        r"(?:(\d+)h)?(?:(\d+)m)?(?:(\d+(?:\.\d+)?)s)?", token, re.I
    )
    if not match or not any(match.groups()):
        raise ValueError(f"invalid time offset: {value!r}")
    hours, minutes, seconds = match.groups()
    return (
        int(hours or 0) * 3600
        + int(minutes or 0) * 60
        + float(seconds or 0)
    )


def parse_video_ref(url: str) -> VideoRef:
    match = VIDEO_ID_RE.search(url)
    time_match = TIME_QUERY_RE.search(url)
    timestamp = parse_offset(time_match.group(1)) if time_match else None
    return VideoRef(url=url, video_id=match.group(1) if match else None,
                    timestamp=timestamp)


def strip_cue_text(line: str) -> str:
    return TAG_RE.sub("", line).replace("&nbsp;", " ").strip()


def parse_cues(text: str) -> list[Cue]:
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    cues: list[Cue] = []
    index = 0
    while index < len(lines):
        raw = lines[index].strip()
        if not raw or VTT_HEADER_RE.match(raw):
            index += 1
            continue
        if "-->" not in raw:
            if index + 1 < len(lines) and "-->" in lines[index + 1]:
                index += 1
                raw = lines[index].strip()
            else:
                index += 1
                continue
        start_raw, end_raw = raw.split("-->", 1)
        start = parse_clock(start_raw.strip().split()[0])
        end = parse_clock(end_raw.strip().split()[0])
        index += 1
        body: list[str] = []
        while index < len(lines) and lines[index].strip():
            cleaned = strip_cue_text(lines[index])
            if cleaned:
                body.append(cleaned)
            index += 1
        joined = " ".join(body)
        if joined:
            cues.append(Cue(start=start, end=end, text=joined))
    return cues


def dedupe_rolling(cues: list[Cue]) -> list[Cue]:
    if not cues:
        return []
    out = [cues[0]]
    for cue in cues[1:]:
        previous = out[-1]
        if cue.text == previous.text:
            out[-1] = Cue(previous.start, max(previous.end, cue.end), cue.text)
        elif cue.text.startswith(previous.text) and len(cue.text) > len(
            previous.text
        ):
            out[-1] = Cue(previous.start, max(previous.end, cue.end), cue.text)
        elif previous.text.startswith(cue.text):
            out[-1] = Cue(
                previous.start, max(previous.end, cue.end), previous.text
            )
        else:
            out.append(cue)
    return out


def slice_cues(cues: list[Cue], around: float, window: float) -> list[Cue]:
    lo = max(0.0, around - window)
    hi = around + window
    return [cue for cue in cues if cue.end >= lo and cue.start <= hi]


def search_cues(cues: list[Cue], query: str) -> list[Cue]:
    needle = query.casefold()
    return [cue for cue in cues if needle in cue.text.casefold()]


def format_clock(seconds: float) -> str:
    total = max(0, int(seconds))
    hours, rem = divmod(total, 3600)
    minutes, secs = divmod(rem, 60)
    if hours:
        return f"{hours:d}:{minutes:02d}:{secs:02d}"
    return f"{minutes:d}:{secs:02d}"


def yt_dlp_command() -> list[str]:
    found = shutil.which("yt-dlp")
    if found:
        return [found]
    if shutil.which("nix"):
        return ["nix", "shell", "nixpkgs#yt-dlp", "-c", "yt-dlp"]
    raise FileNotFoundError("yt-dlp is not installed and nix is unavailable")


def select_caption_file(directory: Path) -> Path | None:
    files = sorted(directory.glob("*.vtt")) + sorted(directory.glob("*.srt"))
    if not files:
        return None
    manual = [path for path in files if "auto" not in path.name.lower()]
    return (manual or files)[0]


def load_info(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def fetch_with_yt_dlp(
    url: str, workdir: Path
) -> tuple[dict[str, Any], Path | None]:
    output = workdir / "%(id)s"
    command = yt_dlp_command() + [
        "--skip-download",
        "--no-playlist",
        "--no-warnings",
        "--write-info-json",
        "--write-subs",
        "--write-auto-subs",
        "--sub-langs",
        "en.*,en",
        "--sub-format",
        "vtt/best",
        "-o",
        str(output),
        url,
    ]
    subprocess.run(command, check=True)
    info_files = list(workdir.glob("*.info.json"))
    if not info_files:
        raise FileNotFoundError("yt-dlp wrote no info JSON")
    return load_info(info_files[0]), select_caption_file(workdir)


def metadata_from_info(info: dict[str, Any], url: str) -> dict[str, Any]:
    chapters = []
    for chapter in info.get("chapters") or []:
        start = float(chapter.get("start_time") or 0)
        chapters.append(
            {
                "start": start,
                "title": chapter.get("title") or "",
                "clock": format_clock(start),
            }
        )
    duration = info.get("duration")
    return {
        "id": info.get("id"),
        "title": info.get("title") or "",
        "channel": info.get("channel") or info.get("uploader") or "",
        "duration": duration,
        "duration_string": info.get("duration_string")
        or (format_clock(duration) if duration else ""),
        "upload_date": info.get("upload_date") or "",
        "url": info.get("webpage_url") or url,
        "description": info.get("description") or "",
        "chapters": chapters,
    }


def render_text(
    meta: dict[str, Any],
    cues: list[Cue],
    around: float | None,
    window: float,
    query: str | None,
) -> str:
    lines = [
        f"Title: {meta.get('title') or '(unknown)'}",
        f"Channel: {meta.get('channel') or '(unknown)'}",
        f"Duration: {meta.get('duration_string') or '(unknown)'}",
        f"URL: {meta.get('url') or ''}",
    ]
    if around is not None:
        lines.append(f"Timestamp: {format_clock(around)} ({around:.0f}s)")
    lines.append("")
    description = (meta.get("description") or "").strip()
    if description:
        lines.append("Description:")
        lines.append(description)
        lines.append("")
    chapters = meta.get("chapters") or []
    if chapters:
        lines.append("Chapters:")
        for chapter in chapters:
            lines.append(f"- {chapter['clock']} {chapter['title']}")
        lines.append("")
    if around is not None and cues:
        clock = format_clock(around)
        lines.append(
            f"Captions around {clock} (±{window:.0f}s):"
        )
        for cue in slice_cues(cues, around, window):
            lines.append(f"[{format_clock(cue.start)}] {cue.text}")
        lines.append("")
    if query:
        hits = search_cues(cues, query)
        lines.append(f"Query matches ({query!r}): {len(hits)}")
        for cue in hits:
            lines.append(f"[{format_clock(cue.start)}] {cue.text}")
        lines.append("")
    if around is None and not query:
        if cues:
            lines.append(
                f"Captions: {len(cues)} cues. Pass --around or --query; "
                "do not dump the full transcript into chat."
            )
            lines.append("Sample:")
            for cue in cues[:12]:
                lines.append(f"[{format_clock(cue.start)}] {cue.text}")
        else:
            lines.append("Captions: none")
    return "\n".join(lines).rstrip() + "\n"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract YouTube title, description and captions."
    )
    parser.add_argument("url", nargs="?", help="YouTube URL")
    parser.add_argument("--url", dest="url_flag", help="YouTube URL")
    parser.add_argument("--around", help="Seconds or 1h2m3s to centre captions")
    parser.add_argument(
        "--window",
        type=float,
        default=90.0,
        help="Seconds either side of --around (default 90)",
    )
    parser.add_argument("--query", help="Case-insensitive caption search")
    parser.add_argument(
        "--subs-file", type=Path, help="Local VTT or SRT fixture"
    )
    parser.add_argument(
        "--info-file", type=Path, help="Local yt-dlp info JSON"
    )
    parser.add_argument(
        "--json", action="store_true", help="JSON output"
    )
    parser.add_argument(
        "--workdir", type=Path, help="Existing yt-dlp directory"
    )
    return parser.parse_args(argv)


def resolve_url(args: argparse.Namespace) -> str:
    url = args.url_flag or args.url
    if not url and not args.subs_file and not args.info_file:
        raise SystemExit(
            "a YouTube URL is required unless fixtures are supplied"
        )
    return url or ""


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    url = resolve_url(args)
    ref = parse_video_ref(url) if url else VideoRef(url="", video_id=None,
                                                    timestamp=None)
    around = parse_offset(args.around) if args.around else ref.timestamp

    info: dict[str, Any] = {}
    caption_text = ""
    if args.info_file or args.subs_file:
        info = load_info(args.info_file) if args.info_file else {}
        if args.subs_file:
            caption_text = args.subs_file.read_text(
                encoding="utf-8", errors="replace"
            )
    else:
        workdir = args.workdir
        cleanup = None
        if workdir is None:
            cleanup = tempfile.TemporaryDirectory(prefix="youtube-extract-")
            workdir = Path(cleanup.name)
        try:
            info, subs_path = fetch_with_yt_dlp(url, workdir)
            if subs_path:
                caption_text = subs_path.read_text(
                    encoding="utf-8", errors="replace"
                )
        finally:
            if cleanup is not None:
                cleanup.cleanup()

    meta = metadata_from_info(info, url)
    cues = dedupe_rolling(parse_cues(caption_text)) if caption_text else []
    if args.json:
        payload = {
            **meta,
            "timestamp": around,
            "window": args.window,
            "query": args.query,
            "cue_count": len(cues),
            "around": [
                asdict(cue) for cue in (
                    slice_cues(cues, around, args.window)
                    if around is not None else []
                )
            ],
            "matches": [
                asdict(cue) for cue in (
                    search_cues(cues, args.query) if args.query else []
                )
            ],
        }
        json.dump(payload, sys.stdout, indent=2, ensure_ascii=False)
        sys.stdout.write("\n")
        return 0
    sys.stdout.write(render_text(meta, cues, around, args.window, args.query))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
