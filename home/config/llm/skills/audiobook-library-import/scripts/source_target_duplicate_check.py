#!/usr/bin/env python3
"""Compare source payload names against Audiobookshelf library target roots.

Used in Phase 1c (source → target duplicate preflight) for Andromeda → Terminus
imports. Matches on basename, normalized title, and ASIN-like tokens found in
folder names. Optional size hints when --source-paths and files exist.

Scan failures return an incomplete report and exit 1, never staging eligibility.
Successful scans cover the configured maximum depth beneath each target root.

No third-party dependencies.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable


ASIN_RE = re.compile(r"\b(B0[A-Z0-9]{8})\b", re.IGNORECASE)
BRACKET_ASIN_RE = re.compile(r"\[(B0[A-Z0-9]{8})\]", re.IGNORECASE)
NOISE_RE = re.compile(
    r"""
    \(unabridged\)|\(abridged\)|\bunabridged\b|\babridged\b|
    \bcomplete\b|\bwebrip\b|\bx264\b|\b720p\b|\b1080p\b|
    \bnf\b|\bgalaxytv\b|\btgx\b
    """,
    re.IGNORECASE | re.VERBOSE,
)
NON_ALNUM_RE = re.compile(r"[^a-z0-9]+")


def extract_asins(text: str) -> set[str]:
    found = set()
    for match in ASIN_RE.finditer(text or ""):
        found.add(match.group(1).upper())
    return found


def normalize_title(name: str) -> str:
    """Lowercase title-ish key: strip ASIN brackets, noise tags, punctuation."""
    s = name or ""
    s = BRACKET_ASIN_RE.sub(" ", s)
    s = NOISE_RE.sub(" ", s)
    s = s.lower().strip()
    # drop extension for file-like basenames
    if "." in Path(s).name and Path(s).suffix.lower() in {
        ".m4b",
        ".m4a",
        ".mp3",
        ".flac",
        ".ogg",
        ".opus",
        ".aac",
        ".wav",
        ".cue",
    }:
        s = str(Path(s).with_suffix(""))
    s = NON_ALNUM_RE.sub(" ", s)
    return " ".join(s.split())


@dataclass
class TargetEntry:
    path: str
    name: str
    normalized: str
    asins: set[str] = field(default_factory=set)
    size_bytes: int | None = None


@dataclass
class SourceEntry:
    name: str
    path: str | None = None
    size_bytes: int | None = None

    @property
    def normalized(self) -> str:
        return normalize_title(self.name)

    @property
    def asins(self) -> set[str]:
        keys = extract_asins(self.name)
        if self.path:
            keys |= extract_asins(self.path)
        return keys


@dataclass
class Match:
    source: str
    target_path: str
    match_type: str  # exact_name | normalized | asin | size
    detail: str


class TargetScanError(RuntimeError):
    def __init__(self, errors: list[dict[str, str]]):
        super().__init__("Target library scan incomplete")
        self.errors = errors


def walk_target(root: Path, max_depth: int, depth: int = 1):
    """Walk the configured depth without suppressing filesystem errors."""
    with os.scandir(root) as scan:
        children = sorted(scan, key=lambda entry: entry.name)
    for entry in children:
        yield Path(entry.path), entry.stat()
        if depth < max_depth and entry.is_dir(follow_symlinks=False):
            yield from walk_target(Path(entry.path), max_depth, depth + 1)


def iter_target_entries(
    roots: Iterable[Path],
    *,
    max_depth: int = 3,
) -> list[TargetEntry]:
    entries: list[TargetEntry] = []
    seen: set[str] = set()
    errors: list[dict[str, str]] = []
    for root in roots:
        root = root.expanduser()
        try:
            if not stat.S_ISDIR(root.stat().st_mode):
                raise NotADirectoryError(f"Not a library directory: {root}")
            for path, info in walk_target(root, max_depth):
                is_file = stat.S_ISREG(info.st_mode)
                if not stat.S_ISDIR(info.st_mode) and not is_file:
                    continue
                if is_file and path.suffix.lower() not in {
                    ".m4b", ".m4a", ".mp3", ".flac", ".ogg", ".opus",
                }:
                    continue
                key = str(path.resolve(strict=True))
                if key in seen:
                    continue
                seen.add(key)
                entries.append(
                    TargetEntry(
                        path=key,
                        name=path.name,
                        normalized=normalize_title(path.name),
                        asins=extract_asins(path.name)
                        | extract_asins(str(path)),
                        size_bytes=info.st_size if is_file else None,
                    )
                )
        except OSError as exc:
            errors.append({"root": str(root), "error": str(exc)})
    if errors:
        raise TargetScanError(errors)
    return entries


def load_sources(
    names: Iterable[str],
    *,
    source_paths: dict[str, Path] | None = None,
) -> list[SourceEntry]:
    out: list[SourceEntry] = []
    for raw in names:
        name = raw.strip()
        if not name or name.startswith("#"):
            continue
        path = None
        size = None
        if source_paths and name in source_paths:
            path = source_paths[name]
        elif source_paths and Path(name).name in source_paths:
            path = source_paths[Path(name).name]
        if path is not None and path.exists():
            try:
                if path.is_file():
                    size = path.stat().st_size
                elif path.is_dir():
                    size = sum(
                        p.stat().st_size for p in path.rglob("*") if p.is_file()
                    )
            except OSError:
                size = None
            path_s = str(path)
        else:
            path_s = None
        out.append(SourceEntry(name=name, path=path_s, size_bytes=size))
    return out


def find_matches(
    sources: list[SourceEntry],
    targets: list[TargetEntry],
) -> list[Match]:
    matches: list[Match] = []
    # Index targets
    by_exact: dict[str, list[TargetEntry]] = {}
    by_norm: dict[str, list[TargetEntry]] = {}
    by_asin: dict[str, list[TargetEntry]] = {}
    for t in targets:
        by_exact.setdefault(t.name.lower(), []).append(t)
        if t.normalized:
            by_norm.setdefault(t.normalized, []).append(t)
        for a in t.asins:
            by_asin.setdefault(a, []).append(t)

    for s in sources:
        seen_paths: set[str] = set()

        def add(match_type: str, target: TargetEntry, detail: str) -> None:
            if target.path in seen_paths:
                return
            seen_paths.add(target.path)
            matches.append(
                Match(
                    source=s.name,
                    target_path=target.path,
                    match_type=match_type,
                    detail=detail,
                )
            )

        for t in by_exact.get(s.name.lower(), []):
            add("exact_name", t, f"basename equals {t.name!r}")

        if s.normalized:
            for t in by_norm.get(s.normalized, []):
                add(
                    "normalized",
                    t,
                    f"normalized {s.normalized!r} == {t.normalized!r}",
                )

        for asin in s.asins:
            for t in by_asin.get(asin, []):
                add("asin", t, f"ASIN {asin}")

        if s.size_bytes is not None and s.size_bytes > 0:
            for t in targets:
                if t.size_bytes == s.size_bytes:
                    add(
                        "size",
                        t,
                        f"same size_bytes={s.size_bytes}",
                    )
    return matches


def format_markdown(
    sources: list[SourceEntry],
    matches: list[Match],
) -> str:
    by_source: dict[str, list[Match]] = {s.name: [] for s in sources}
    for m in matches:
        by_source.setdefault(m.source, []).append(m)

    lines = [
        "# Source → target duplicate preflight",
        "",
        f"Sources checked: **{len(sources)}**",
        (
            "Sources with at least one hit: "
            f"**{sum(1 for v in by_source.values() if v)}**"
        ),
        f"Total match rows: **{len(matches)}**",
        "",
        "| Source | Hits | Match types | Example target |",
        "|---|---|---|---|",
    ]
    for s in sources:
        hits = by_source.get(s.name, [])
        if not hits:
            lines.append(f"| {s.name} | 0 | — | — |")
            continue
        types = ",".join(sorted({h.match_type for h in hits}))
        example = hits[0].target_path
        lines.append(f"| {s.name} | {len(hits)} | {types} | `{example}` |")

    lines += ["", "## Match detail", ""]
    for s in sources:
        hits = by_source.get(s.name, [])
        if not hits:
            continue
        lines.append(f"### {s.name}")
        for h in hits:
            lines.append(
                f"- **{h.match_type}**: `{h.target_path}` — {h.detail}"
            )
        lines.append("")

    clean = [s.name for s in sources if not by_source.get(s.name)]
    if clean:
        lines += ["## No hit (eligible to stage pending full AC5)", ""]
        for name in clean:
            lines.append(f"- {name}")
        lines.append("")
    return "\n".join(lines)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=(
            "Preflight source payload names against library target roots "
            "for duplicate detection."
        )
    )
    p.add_argument(
        "--sources-file",
        type=Path,
        required=True,
        help="File of source names (one per line), or '-' for stdin",
    )
    p.add_argument(
        "--targets",
        type=Path,
        nargs="+",
        required=True,
        help="Library root directories to scan",
    )
    p.add_argument(
        "--source-root",
        type=Path,
        help="Optional root to resolve source names for size hints",
    )
    p.add_argument(
        "--max-depth",
        type=int,
        default=3,
        help="Max depth under each target root (default 3)",
    )
    p.add_argument(
        "--format",
        choices=("markdown", "json"),
        default="markdown",
    )
    p.add_argument("-o", "--output", type=Path)
    return p


def read_sources_file(path: Path) -> list[str]:
    if str(path) == "-":
        return sys.stdin.read().splitlines()
    return path.read_text(encoding="utf-8").splitlines()


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    if args.max_depth < 1:
        parser.error("--max-depth must be at least 1")
    names = read_sources_file(args.sources_file)
    source_paths: dict[str, Path] = {}
    if args.source_root and args.source_root.is_dir():
        for name in names:
            name = name.strip()
            if not name:
                continue
            candidate = args.source_root / name
            if candidate.exists():
                source_paths[name] = candidate

    sources = load_sources(names, source_paths=source_paths or None)
    scan_errors: list[dict[str, str]] = []
    try:
        targets = iter_target_entries(args.targets, max_depth=args.max_depth)
        matches = find_matches(sources, targets)
    except TargetScanError as exc:
        scan_errors = exc.errors
        matches = []

    if args.format == "json":
        payload: str = json.dumps(
            {
                "status": "incomplete" if scan_errors else "complete",
                "scan_errors": scan_errors,
                "max_depth": args.max_depth,
                "sources": [asdict(s) for s in sources],
                "match_count": None if scan_errors else len(matches),
                "matches": [asdict(m) for m in matches],
            },
            indent=2,
        ) + "\n"
    elif scan_errors:
        lines = [
            "# Source to target duplicate preflight incomplete", "",
            "No staging decision is available until every target is scanned.",
            "",
        ]
        lines.extend(
            f"- `{error['root']}`: {error['error']}" for error in scan_errors
        )
        payload = "\n".join(lines) + "\n"
    else:
        payload = format_markdown(sources, matches)

    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        sys.stdout.write(payload)
    return 1 if scan_errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
