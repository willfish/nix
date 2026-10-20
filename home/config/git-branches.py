#!/usr/bin/env python3
"""Read-only branch overview for the Home Manager git branches alias."""

import argparse
import os
import shutil
import subprocess
import sys
import textwrap
import unicodedata
from dataclasses import dataclass


def git(*args, check=True):
    return subprocess.run(
        ["git", "--no-pager", *args],
        capture_output=True,
        text=True,
        errors="replace",
        env={**os.environ, "LC_ALL": "C"},
        check=check,
    )


def safe(text):
    # Commit messages and paths must not emit terminal control sequences.
    return "".join(char if char.isprintable() else " " for char in text)


def cell_width(text):
    return sum(
        0
        if unicodedata.combining(char)
        else 2
        if unicodedata.east_asian_width(char) in "WF"
        else 1
        for char in text
    )


def fit(text, width):
    if cell_width(text) > width:
        shortened = ""
        for char in text:
            if cell_width(shortened + char) > width - 3:
                break
            shortened += char
        text = shortened + "..."
    return text + " " * max(0, width - cell_width(text))


@dataclass
class Branch:
    name: str
    state: str
    updated: str
    upstream: str
    subject: str
    warning: bool


def branches():
    fields = (
        "refname",
        "HEAD",
        "worktreepath",
        "upstream",
        "upstream:track",
        "committerdate:relative",
        "subject",
        "symref",
    )
    result = git(
        "for-each-ref",
        "--sort=-committerdate",
        "--format=" + "%00".join(f"%({field})" for field in fields) + "%00",
        "refs/heads",
        "refs/remotes",
    )
    local, remote = [], []
    # NUL fields also preserve worktree paths that contain newlines.
    values = result.stdout.split("\0")
    for index in range(0, len(values) - 1, len(fields)):
        ref, head, path, upstream, track, updated, subject, symref = values[
            index : index + len(fields)
        ]
        ref = ref.lstrip("\n")
        if symref:
            continue  # origin/HEAD is a pointer, not another branch.
        is_remote = ref.startswith("refs/remotes/")
        prefix = "refs/remotes/" if is_remote else "refs/heads/"
        state = "current" if head == "*" else "worktree" if path else ""
        if not upstream:
            sync = "no upstream"
        else:
            upstream = upstream.removeprefix("refs/remotes/").removeprefix(
                "refs/heads/"
            )
            status = track.strip("[]") or "in sync"
            if status == "gone":
                status = "upstream gone"
            sync = f"{status} | {upstream}"
        branch = Branch(
            safe(ref.removeprefix(prefix)),
            state,
            safe(updated),
            safe(sync),
            safe(subject),
            bool(track),
        )
        (remote if is_remote else local).append(branch)
    local.sort(key=lambda branch: branch.state != "current")
    return local, remote


class View:
    def __init__(self, args):
        self.color = args.color == "always" or (
            args.color == "auto"
            and sys.stdout.isatty()
            and not os.environ.get("NO_COLOR")
            and os.environ.get("TERM") != "dumb"
        )
        self.width = shutil.get_terminal_size((100, 24)).columns
        self.full = args.full or not sys.stdout.isatty()

    def style(self, text, code):
        return f"\033[{code}m{text}\033[0m" if self.color else text

    def wrapped(self, text, indent="", code="0"):
        # Reserve space for wide glyphs as well as ordinary ASCII text.
        line = indent
        for word in textwrap.wrap(text, width=max(1, self.width - len(indent))):
            for char in word:
                if cell_width(line + char) > self.width:
                    print(self.style(line, code))
                    line = indent
                line += char
            print(self.style(line, code))
            line = indent

    def section(self, title, items, remote=False):
        print(self.style(f"{title} ({len(items)})", "1"))
        if not items:
            print(
                "  No remote-tracking branches."
                if remote
                else "  No local branches yet."
            )
            print()
            return

        if self.width < 110 and not self.full:
            for branch in items:
                label = f"  [{branch.state}]" if branch.state else ""
                self.wrapped(
                    branch.name + label,
                    code="1;32" if branch.state == "current" else "36",
                )
                detail = branch.updated
                if not remote:
                    detail += " | " + branch.upstream
                self.wrapped(detail, "  ")
            print()
            return

        headers = (
            ["BRANCH", "UPDATED", "LAST COMMIT"]
            if remote
            else [
                "STATE",
                "BRANCH",
                "UPDATED",
                "UPSTREAM / SYNC",
                "LAST COMMIT",
            ]
        )
        rows = [
            [branch.name, branch.updated, branch.subject]
            if remote
            else [
                branch.state,
                branch.name,
                branch.updated,
                branch.upstream,
                branch.subject,
            ]
            for branch in items
        ]
        widths = [
            max(cell_width(row[i]) for row in [headers, *rows])
            for i in range(len(headers))
        ]
        if not self.full:
            if remote:
                widths = [
                    min(widths[0], self.width // 3),
                    min(widths[1], 14),
                    0,
                ]
            else:
                widths = [
                    8,
                    min(widths[1], self.width // 4),
                    min(widths[2], 14),
                    min(widths[3], self.width // 3),
                    0,
                ]
            widths[-1] = self.width - sum(widths[:-1]) - 2 * (len(headers) - 1)
        print(
            self.style(
                "  ".join(
                    fit(value, width) for value, width in zip(headers, widths)
                ).rstrip(),
                "1",
            )
        )
        for branch, row in zip(items, rows):
            cells = [fit(value, width) for value, width in zip(row, widths)]
            name_index = 0 if remote else 1
            cells[name_index] = self.style(
                cells[name_index], "1" if branch.state == "current" else "36"
            )
            if not remote:
                if branch.state:
                    cells[0] = self.style(
                        cells[0], "32" if branch.state == "current" else "36"
                    )
                if branch.warning:
                    cells[3] = self.style(cells[3], "33")
            print("  ".join(cells).rstrip())
        print()


def main():
    parser = argparse.ArgumentParser(
        prog="git branches",
        description="Show branches, worktrees, upstream sync and commits.",
        epilog="Read-only. Remote state is from your last fetch; "
        "this command never fetches. Updated means the tip commit's "
        "committer date, not last checkout or use.",
    )
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument(
        "-a",
        "--all",
        action="store_true",
        help="include remote-tracking branches",
    )
    scope.add_argument(
        "-r",
        "--remotes",
        action="store_true",
        help="show only remote-tracking branches",
    )
    parser.add_argument(
        "--full",
        action="store_true",
        help="show all columns without truncation",
    )
    parser.add_argument(
        "--color", choices=("auto", "always", "never"), default="auto"
    )
    args = parser.parse_args()
    git("rev-parse", "--git-dir")
    local, remote = branches()
    view = View(args)
    head = git("symbolic-ref", "--quiet", "--short", "HEAD", check=False)
    if head.returncode == 1:
        commit = git("rev-parse", "--short", "HEAD").stdout.strip()
        view.wrapped(f"HEAD detached at {commit}; no current branch.")
        print()
    elif head.returncode:
        raise subprocess.CalledProcessError(
            head.returncode, head.args, stderr=head.stderr
        )
    elif not any(branch.state == "current" for branch in local):
        view.wrapped(
            f"Current branch: {safe(head.stdout.strip())} (no commits yet)"
        )
        print()
    if not args.remotes:
        view.section("Local branches", local)
    if args.all or args.remotes:
        view.section("Remote branches (last fetched)", remote, remote=True)
    if not args.remotes and any(branch.state == "worktree" for branch in local):
        view.wrapped(
            "worktree = checked out elsewhere; paths: git worktree list"
        )
    view.wrapped("Sync uses locally stored refs; no fetch is performed.")
    if not args.all and not args.remotes:
        view.wrapped("Also: git branches --all | --remotes | --full")
    elif not view.full:
        view.wrapped(
            "Full names and commit details: git branches --full"
            + (" --remotes" if args.remotes else " --all")
        )


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f"git branches: {safe(error.stderr.strip())}", file=sys.stderr)
        sys.exit(error.returncode)
    except BrokenPipeError:
        sys.stdout = open(os.devnull, "w")
        sys.exit(0)
    except KeyboardInterrupt:
        sys.exit(130)
