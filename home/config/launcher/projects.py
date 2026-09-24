#!/usr/bin/env python3
"""Bounded project catalogue for an Elephant menu.

Discovery only stats directories. It does not run git, read environment
files, or execute repository code. Project values are opaque ids, not paths.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys


MAX_DEPTH = 3
MAX_ENTRIES = 200
# Every Elephant query rescans. Cap directory entries read, not only results.
MAX_DIR_ENTRIES = 4000
ICON = "folder"
DOTFILES_NAME = ".dotfiles"
REPOSITORIES_NAME = "Repositories"
ACTIONS = ("terminal", "editor", "files")
# Output and package directories, not project roots. Hidden names are skipped
# separately, so this list is only the visible dependency directory names.
DEPENDENCY_DIRS = frozenset(
    {
        "node_modules",
        "bower_components",
        "vendor",
        "venv",
        "site-packages",
        "__pycache__",
        "third_party",
        "third-party",
        "deps",
        "Pods",
        "Carthage",
        "elm-stuff",
        "result",
        "target",
        "dist",
        "build",
    }
)
_ID_PREFIX = b"launcher-project\0"


def absolute_home(home):
    return Path(os.path.abspath(os.fspath(home)))


def selected_home(explicit=None):
    if explicit:
        return absolute_home(explicit)
    env = os.environ.get("LAUNCHER_HOME")
    if env:
        return absolute_home(env)
    return absolute_home(Path.home())


def project_id(path):
    return hashlib.sha256(
        _ID_PREFIX + os.fsencode(Path(os.fspath(path)))
    ).hexdigest()


def _valid_id(value):
    if len(value) != 64:
        return False
    return all(character in "0123456789abcdef" for character in value)


def _safe_text(value):
    return (
        os.fsdecode(os.fsencode(value))
        .encode("utf-8", "replace")
        .decode("utf-8")
    )


def _is_real_dir(path):
    try:
        info = os.lstat(path)
    except OSError:
        return False
    return stat.S_ISDIR(info.st_mode)


def _has_git_marker(path):
    """True when .git is a file or directory. Symlinks are not followed."""
    try:
        info = os.lstat(path / ".git")
    except OSError:
        return False
    return stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)


def _skipped_name(name):
    return (
        name in {".", ".."}
        or name.startswith(".")
        or name in DEPENDENCY_DIRS
        or "/" in name
        or "\x00" in name
    )


class _Budget:
    def __init__(self, limit):
        self.limit = limit
        self.used = 0

    def allow(self):
        if self.used >= self.limit:
            return False
        self.used += 1
        return True

    def exhausted(self):
        return self.used >= self.limit


def _candidate_names(directory, budget):
    """Read only the remaining dirent budget, not the whole directory."""
    try:
        iterator = os.scandir(directory)
    except OSError:
        return []
    names = []
    try:
        while not budget.exhausted():
            try:
                entry = next(iterator)
            except StopIteration:
                break
            budget.allow()
            name = entry.name
            if _skipped_name(name):
                continue
            try:
                if entry.is_symlink() or not entry.is_dir(
                    follow_symlinks=False
                ):
                    continue
            except OSError:
                continue
            names.append(name)
    finally:
        iterator.close()
    names.sort(key=os.fsencode)
    return names


def _scan_repositories(root, found, budget):
    def walk(directory, depth):
        if len(found) >= MAX_ENTRIES or budget.exhausted():
            return
        names = _candidate_names(directory, budget)
        for name in names:
            if len(found) >= MAX_ENTRIES:
                return
            child = directory / name
            if not _is_real_dir(child):
                continue
            if _has_git_marker(child):
                found.append(child)
                continue
            if depth < MAX_DEPTH and not budget.exhausted():
                walk(child, depth + 1)

    walk(root, 1)


def discover_projects(home):
    """Return project directories under home, bounded and symlink-safe."""
    home = absolute_home(home)
    projects = []
    dotfiles = home / DOTFILES_NAME
    if _is_real_dir(dotfiles):
        projects.append(dotfiles)
    repositories = home / REPOSITORIES_NAME
    if _is_real_dir(repositories) and len(projects) < MAX_ENTRIES:
        _scan_repositories(repositories, projects, _Budget(MAX_DIR_ENTRIES))
    unique = []
    seen = set()
    for path in projects:
        if len(unique) >= MAX_ENTRIES:
            break
        key = os.fsencode(path)
        if key in seen:
            continue
        seen.add(key)
        unique.append(path)
    unique.sort(key=os.fsencode)
    return unique[:MAX_ENTRIES]


def catalogue_entry(home, path):
    home = absolute_home(home)
    relative = path.relative_to(home)
    keywords = []
    seen = set()
    for part in relative.parts:
        for candidate in (part, part[1:] if part.startswith(".") else part):
            if candidate and candidate not in seen:
                seen.add(candidate)
                keywords.append(_safe_text(candidate))
    return {
        "Text": _safe_text(path.name),
        "Subtext": _safe_text(relative.as_posix()),
        "Value": project_id(path),
        "Icon": ICON,
        "Keywords": keywords,
    }


def catalogue_entries(home):
    home = absolute_home(home)
    return [catalogue_entry(home, path) for path in discover_projects(home)]


def launch_argv(action, path):
    directory = os.fsdecode(Path(os.fspath(path)))
    if action == "terminal":
        return ["ghostty", f"--working-directory={directory}"]
    if action == "editor":
        return [
            "ghostty",
            f"--working-directory={directory}",
            "-e",
            "nvim",
            ".",
        ]
    if action == "files":
        return ["xdg-open", directory]
    raise ValueError("invalid action")


def _under(path, root):
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None
    if any(part in {".", ".."} for part in relative.parts):
        return None
    return relative


def launchable(home, path):
    """Reject missing, escaped, hidden, or non-project paths before launch."""
    home = absolute_home(home)
    raw = Path(os.fspath(path))
    if not raw.is_absolute() or any(part in {".", ".."} for part in raw.parts):
        return False
    path = Path(os.path.abspath(raw))
    dotfiles = home / DOTFILES_NAME
    if path == dotfiles:
        return _is_real_dir(path)
    relative = _under(path, home / REPOSITORIES_NAME)
    if (
        relative is None
        or not relative.parts
        or len(relative.parts) > MAX_DEPTH
    ):
        return False
    if any(_skipped_name(part) for part in relative.parts):
        return False
    current = home / REPOSITORIES_NAME
    if not _is_real_dir(current):
        return False
    for part in relative.parts:
        current = current / part
        if not _is_real_dir(current):
            return False
    return current == path and _has_git_marker(path)


# setsid stays in Elephant's cgroup. A user scope survives Elephant restarts.
SYSTEMD_RUN_PREFIX = (
    "systemd-run",
    "--user",
    "--scope",
    "--collect",
    "--quiet",
    "--",
)


def spawn(argv):
    subprocess.Popen(
        list(SYSTEMD_RUN_PREFIX) + list(argv),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
        close_fds=True,
        shell=False,
        cwd="/",
    )


def _fail(message):
    print(message, file=sys.stderr)
    return 1


def open_project(home, identifier, action):
    if action not in ACTIONS:
        return _fail("invalid action")
    if not _valid_id(identifier):
        return _fail("unknown project")
    home = absolute_home(home)
    selected = None
    for path in discover_projects(home):
        if project_id(path) == identifier:
            selected = path
            break
    if selected is None or not launchable(home, selected):
        return _fail("unknown project")
    try:
        spawn(launch_argv(action, selected))
    except OSError:
        return _fail("unable to launch")
    return 0


def list_projects(home):
    json.dump(catalogue_entries(home), sys.stdout, ensure_ascii=False)
    sys.stdout.write("\n")
    return 0


def _parser():
    parser = argparse.ArgumentParser(description="Bounded project catalogue")
    parser.add_argument("--home", help="home directory override")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    opened = commands.add_parser("open")
    opened.add_argument("project_id")
    opened.add_argument("action")
    return parser


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    try:
        args = _parser().parse_args(argv)
    except SystemExit as exc:
        return int(exc.code or 0)
    home = selected_home(args.home)
    if args.command == "list":
        try:
            return list_projects(home)
        except (OSError, ValueError, UnicodeError):
            return _fail("unable to list")
    return open_project(home, args.project_id, args.action)


if __name__ == "__main__":
    sys.exit(main())
