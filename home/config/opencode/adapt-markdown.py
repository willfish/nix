#!/usr/bin/env python3
"""Rewrite Pi agent and command markdown for OpenCode's strict frontmatter."""

from __future__ import annotations

import argparse
from pathlib import Path

AGENT_KEYS = {"description", "model", "mode"}
COMMAND_KEYS = {"description", "agent", "model", "subtask"}


def split_frontmatter(text: str) -> tuple[list[str], str]:
    if not text.startswith("---\n"):
        return [], text
    end = text.find("\n---\n", 4)
    if end < 0:
        return [], text
    lines = text[4:end].splitlines()
    body = text[end + 5 :]
    return lines, body


def parse_simple(lines: list[str]) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in lines:
        if not line.strip() or line.startswith(" ") or ":" not in line:
            continue
        key, value = line.split(":", 1)
        fields[key.strip()] = value.strip()
    return fields


def render(fields: dict[str, str], order: list[str], body: str) -> str:
    lines = ["---"]
    for key in order:
        if key in fields and fields[key] != "":
            lines.append(f"{key}: {fields[key]}")
    lines.extend(["---", ""])
    return "\n".join(lines) + body.lstrip("\n")


def adapt(text: str, kind: str) -> str:
    raw_lines, body = split_frontmatter(text)
    parsed = parse_simple(raw_lines)
    if kind == "command":
        body = body.replace("$@", "$ARGUMENTS")
        fields = {key: parsed[key] for key in COMMAND_KEYS if key in parsed}
        return render(
            fields, ["description", "agent", "model", "subtask"], body
        )
    fields = {key: parsed[key] for key in AGENT_KEYS if key in parsed}
    fields.setdefault("mode", "subagent")
    skills = parsed.get("skills", "").strip("[] ")
    if skills:
        names = ", ".join(
            part.strip() for part in skills.split(",") if part.strip()
        )
        note = (
            "Load these skills when the task reaches their trigger: "
            f"{names}.\n"
        )
        if note.strip() not in body:
            body = body.rstrip() + "\n\n" + note
    return render(fields, ["description", "mode", "model"], body)


def convert_tree(source: Path, destination: Path, kind: str) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for path in sorted(source.glob("*.md")):
        (destination / path.name).write_text(
            adapt(path.read_text(), kind), encoding="utf-8"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--kind", choices=("agent", "command"), required=True)
    args = parser.parse_args()
    convert_tree(args.source, args.destination, args.kind)


if __name__ == "__main__":
    main()
