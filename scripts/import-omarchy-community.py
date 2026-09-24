#!/usr/bin/env python3
"""Refresh declarations from a downloaded, pinned omarchy-site themes page.

Usage: python scripts/import-omarchy-community.py PAGE.html SOURCE_URL
Only parses repository links and labels. Never executes theme repository code.
"""
import json
from html.parser import HTMLParser
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
START = "    # BEGIN generated Omarchy community inputs"
END = "    # END generated Omarchy community inputs"


class Catalogue(HTMLParser):
    def __init__(self):
        super().__init__()
        self.current = None
        self.caption = False
        self.entries = {}

    def handle_starttag(self, tag, attributes):
        attrs = dict(attributes)
        if tag == "figure":
            self.current = {}
        elif self.current is not None:
            if tag == "img":
                src = attrs.get("src", "")
                self.current["slug"] = Path(src).stem
            elif tag == "figcaption":
                self.caption = True
            elif tag == "a" and self.caption:
                self.current["url"] = attrs.get("href", "").rstrip("/")

    def handle_data(self, data):
        if self.current is not None and self.caption:
            self.current["label"] = self.current.get("label", "") + data

    def handle_endtag(self, tag):
        if tag == "figcaption":
            self.caption = False
        elif tag == "figure" and self.current is not None:
            item = self.current
            slug, url = item["slug"], item["url"]
            if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", slug):
                raise ValueError(f"Unsafe theme ID: {slug!r}")
            if len(slug) > 53 or slug in self.entries:
                raise ValueError(f"Duplicate or oversized theme ID: {slug}")
            if not re.fullmatch(
                r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", url):
                raise ValueError(f"Unsupported repository URL: {url}")
            label = item["label"].strip()
            if not label or any(ord(c) < 32 for c in label):
                raise ValueError(f"Invalid theme label: {slug}")
            self.entries[slug] = {"label": label, "url": url}
            self.current = None


def main():
    page, source = sys.argv[1:]
    parser = Catalogue()
    parser.feed(Path(page).read_text())
    if not parser.entries:
        raise ValueError("No community themes found; refusing empty catalogue")
    flake = ROOT / "flake.nix"
    text = flake.read_text()
    if text.count(START) != 1 or text.count(END) != 1:
        raise ValueError("Expected exactly one generated input block")
    entries = dict(sorted(parser.entries.items()))
    unavailable = json.loads(
        (ROOT / "home/user/themes/community-unavailable.json").read_text()
    )
    blocks = [START]
    for slug, item in entries.items():
        if slug in unavailable:
            continue
        blocks.extend([
            f"    omarchy-theme-{slug} = {{",
            f'      url = "git+{item["url"]}?shallow=1";',
            "      flake = false;",
            "    };",
        ])
    blocks.append(END)
    before, rest = text.split(START)
    _, after = rest.split(END)
    flake.write_text(before + "\n".join(blocks) + after)
    target = ROOT / "home/user/themes/community.json"
    target.write_text(json.dumps(
        {"source": source, "themes": entries}, indent=2,
        ensure_ascii=False,
    ) + "\n")
    print(f"Read {len(entries)} community themes; run nix flake lock")
    for slug, reason in unavailable.items():
        print(f"Unavailable: {slug}: {reason}")


if __name__ == "__main__":
    main()
