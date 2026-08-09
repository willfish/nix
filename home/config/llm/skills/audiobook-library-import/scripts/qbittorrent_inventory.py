#!/usr/bin/env python3
"""Inventory qBittorrent payloads from fast-resume files (Andromeda).

Source of truth for download scope is BT_backup/*.fastresume, not a recursive
walk of ~/Downloads. Classifies each torrent as complete / incomplete /
missing_path and emits human or NUL-delimited path lists for rsync.

No third-party deps: includes a minimal bencode decoder for fast-resume.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator


DEFAULT_FASTRESUME_DIR = Path.home() / ".local/share/qBittorrent/BT_backup"
DEFAULT_DOWNLOAD_ROOT = Path.home() / "Downloads"

AUDIO_SUFFIXES = {
    ".m4b",
    ".m4a",
    ".mp3",
    ".flac",
    ".ogg",
    ".opus",
    ".aac",
    ".wav",
}


# --- minimal bencode (decode only) -------------------------------------------


class BencodeError(ValueError):
    pass


def bdecode(data: bytes) -> Any:
    value, idx = _bdecode_at(data, 0)
    if idx != len(data):
        # qBittorrent fast-resume is a single top-level dict; tolerate trailing
        # junk only if we already got a value (should not happen).
        pass
    return value


def _bdecode_at(data: bytes, idx: int) -> tuple[Any, int]:
    if idx >= len(data):
        raise BencodeError("unexpected end of data")
    ch = data[idx : idx + 1]
    if ch == b"i":
        end = data.index(b"e", idx)
        num = int(data[idx + 1 : end])
        return num, end + 1
    if ch == b"l":
        idx += 1
        out: list[Any] = []
        while data[idx : idx + 1] != b"e":
            item, idx = _bdecode_at(data, idx)
            out.append(item)
        return out, idx + 1
    if ch == b"d":
        idx += 1
        out_d: dict[bytes, Any] = {}
        while data[idx : idx + 1] != b"e":
            key, idx = _bdecode_at(data, idx)
            if not isinstance(key, (bytes, bytearray)):
                raise BencodeError("dict key must be bytes")
            val, idx = _bdecode_at(data, idx)
            out_d[bytes(key)] = val
        return out_d, idx + 1
    if ch.isdigit():
        colon = data.index(b":", idx)
        length = int(data[idx:colon])
        start = colon + 1
        end = start + length
        if end > len(data):
            raise BencodeError("string length exceeds data")
        return data[start:end], end
    raise BencodeError(f"invalid bencode at {idx}: {ch!r}")


def _as_str(value: Any) -> str | None:
    if value is None:
        return None
    if isinstance(value, (bytes, bytearray)):
        return value.decode("utf-8", errors="surrogateescape")
    if isinstance(value, str):
        return value
    return str(value)


def _as_int(value: Any, default: int = 0) -> int:
    if value is None:
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def pieces_complete(pieces: Any) -> bool | None:
    """True if all piece bits set; False if any clear; None if unknown."""
    if not isinstance(pieces, (bytes, bytearray)) or not pieces:
        return None
    for i, byte in enumerate(pieces):
        if i < len(pieces) - 1 and byte != 0xFF:
            return False
        if i < len(pieces) - 1:
            continue
        # Last byte may include padding; finished_time is primary.
        if byte == 0xFF:
            return True
        if byte == 0:
            return False
        return None
    if all(b == 0xFF for b in pieces):
        return True
    return False


@dataclass
class TorrentRecord:
    name: str
    save_path: str
    path: str
    exists: bool
    status: str  # complete | incomplete | missing_path | unknown
    finished_time: int
    paused: bool
    total_downloaded: int
    size_bytes: int | None
    is_audiobook_like: bool
    fastresume: str

    def relative_name(self) -> str:
        return self.name


def safe_join_download(root: Path, name: str) -> Path | None:
    """Join name under root; reject absolute and .. traversal."""
    if not name or name.startswith("/") or name.startswith("\\"):
        return None
    # Normalize separators but reject parent traversal
    parts = Path(name).parts
    if any(p in ("..", "") for p in parts):
        # empty parts from leading ./ only — Path("foo/bar").parts is fine
        if ".." in parts:
            return None
    candidate = (root / name).resolve()
    root_resolved = root.resolve()
    try:
        candidate.relative_to(root_resolved)
    except ValueError:
        return None
    return candidate


def looks_audiobook_like(path: Path, name: str) -> bool:
    lowered = name.lower()
    if any(lowered.endswith(sfx) for sfx in AUDIO_SUFFIXES):
        return True
    keywords = (
        "audiobook",
        "unabridged",
        "abridged",
        "m4b",
        "great courses",
        "bbc r4",
        "radio drama",
    )
    if any(k in lowered for k in keywords):
        return True
    if not path.exists():
        return any(lowered.endswith(sfx) for sfx in AUDIO_SUFFIXES)
    if path.is_file():
        return path.suffix.lower() in AUDIO_SUFFIXES
    if path.is_dir():
        try:
            for child in path.rglob("*"):
                if child.is_file() and child.suffix.lower() in AUDIO_SUFFIXES:
                    return True
                # limit walk cost
                if child.is_dir() and child != path:
                    pass
        except OSError:
            return False
    return False


def classify_status(
    *,
    exists: bool,
    finished_time: int,
    pieces: Any,
    total_downloaded: int,
) -> str:
    if not exists:
        return "missing_path"
    if finished_time > 0:
        return "complete"
    pc = pieces_complete(pieces)
    if pc is True and total_downloaded > 0:
        return "complete"
    if pc is False:
        return "incomplete"
    if total_downloaded <= 0:
        return "incomplete"
    return "unknown"


def parse_fastresume(
    fr_path: Path,
    download_root: Path,
) -> TorrentRecord | None:
    try:
        raw = fr_path.read_bytes()
        data = bdecode(raw)
    except (OSError, BencodeError, ValueError):
        return None
    if not isinstance(data, dict):
        return None

    name = _as_str(data.get(b"name") or data.get(b"qBt-name"))
    if not name:
        return None
    save = _as_str(data.get(b"qBt-savePath") or data.get(b"save_path")) or str(
        download_root
    )
    # Prefer configured download root when save path matches default layout
    root = Path(save) if save else download_root
    joined = safe_join_download(root, name)
    if joined is None:
        return None

    finished_time = _as_int(data.get(b"finished_time"))
    paused = bool(_as_int(data.get(b"paused")))
    total_downloaded = _as_int(data.get(b"total_downloaded"))
    pieces = data.get(b"pieces")
    exists = joined.exists()
    size_bytes: int | None = None
    if exists:
        try:
            if joined.is_file():
                size_bytes = joined.stat().st_size
            else:
                size_bytes = sum(
                    p.stat().st_size for p in joined.rglob("*") if p.is_file()
                )
        except OSError:
            size_bytes = None

    status = classify_status(
        exists=exists,
        finished_time=finished_time,
        pieces=pieces,
        total_downloaded=total_downloaded,
    )
    return TorrentRecord(
        name=name,
        save_path=str(root),
        path=str(joined),
        exists=exists,
        status=status,
        finished_time=finished_time,
        paused=paused,
        total_downloaded=total_downloaded,
        size_bytes=size_bytes,
        is_audiobook_like=looks_audiobook_like(joined, name),
        fastresume=str(fr_path),
    )


def inventory(
    fastresume_dir: Path,
    download_root: Path,
    *,
    audiobook_only: bool = False,
) -> list[TorrentRecord]:
    records: list[TorrentRecord] = []
    if not fastresume_dir.is_dir():
        return records
    for fr in sorted(fastresume_dir.glob("*.fastresume")):
        rec = parse_fastresume(fr, download_root)
        if rec is None:
            continue
        if audiobook_only and not rec.is_audiobook_like:
            continue
        records.append(rec)
    return records


def transfer_ready(records: Iterable[TorrentRecord]) -> list[TorrentRecord]:
    """Payloads safe to stage: complete and path present."""
    return [r for r in records if r.status == "complete" and r.exists]


def format_markdown_report(records: list[TorrentRecord]) -> str:
    lines = [
        "# qBittorrent import candidates",
        "",
        f"Total torrents inventoried: **{len(records)}**",
        "",
        "| Name | Status | Exists | Size | Audiobook-like | Path |",
        "|---|---|---|---|---|---|",
    ]
    for r in records:
        size = (
            f"{r.size_bytes:,}" if r.size_bytes is not None else "—"
        )
        lines.append(
            f"| {r.name} | {r.status} | {r.exists} | {size} | "
            f"{r.is_audiobook_like} | `{r.path}` |"
        )
    ready = transfer_ready(records)
    incomplete = [r for r in records if r.status == "incomplete"]
    missing = [r for r in records if r.status == "missing_path"]
    lines += [
        "",
        "## Disposition hints",
        "",
        f"- **import candidate (complete + present):** {len(ready)}",
        f"- **incomplete (do not copy yet):** {len(incomplete)}",
        f"- **missing path (skip / re-check):** {len(missing)}",
        "",
    ]
    if ready:
        lines.append("### Import candidates")
        for r in ready:
            lines.append(f"- `{r.name}` — `{r.path}`")
        lines.append("")
    if incomplete:
        lines.append("### Incomplete")
        for r in incomplete:
            lines.append(f"- `{r.name}` — status={r.status}")
        lines.append("")
    if missing:
        lines.append("### Missing path")
        for r in missing:
            lines.append(f"- `{r.name}` — expected `{r.path}`")
        lines.append("")
    return "\n".join(lines)


def emit_nul_list(
    records: Iterable[TorrentRecord],
    *,
    relative: bool = True,
) -> bytes:
    out = bytearray()
    for r in records:
        payload = r.name if relative else r.path
        out.extend(payload.encode("utf-8", errors="surrogateescape"))
        out.append(0)
    return bytes(out)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Inventory qBittorrent torrents from fast-resume files."
    )
    p.add_argument(
        "--fastresume-dir",
        type=Path,
        default=DEFAULT_FASTRESUME_DIR,
        help="Directory containing *.fastresume",
    )
    p.add_argument(
        "--download-root",
        type=Path,
        default=DEFAULT_DOWNLOAD_ROOT,
        help="Fallback download root if save path missing",
    )
    p.add_argument(
        "--audiobook-only",
        action="store_true",
        help="Keep only names/paths that look like audiobooks",
    )
    p.add_argument(
        "--transfer-ready-only",
        action="store_true",
        help="Only complete torrents whose path exists",
    )
    p.add_argument(
        "--format",
        choices=("markdown", "json", "nul", "names"),
        default="markdown",
        help="Output format (nul = relative names for rsync --from0)",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Write output to file instead of stdout",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    records = inventory(
        args.fastresume_dir,
        args.download_root,
        audiobook_only=args.audiobook_only,
    )
    if args.transfer_ready_only:
        records = transfer_ready(records)

    if args.format == "markdown":
        payload: str | bytes = format_markdown_report(records)
    elif args.format == "json":
        payload = json.dumps([asdict(r) for r in records], indent=2) + "\n"
    elif args.format == "nul":
        payload = emit_nul_list(records, relative=True)
    else:  # names
        payload = "\n".join(r.name for r in records) + ("\n" if records else "")

    if args.output:
        if isinstance(payload, bytes):
            args.output.write_bytes(payload)
        else:
            args.output.write_text(payload, encoding="utf-8")
    else:
        if isinstance(payload, bytes):
            sys.stdout.buffer.write(payload)
        else:
            sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
