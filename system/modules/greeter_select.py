#!/usr/bin/env python3
"""Choose an allowlisted greeter theme without following links or blocking."""

import argparse
import json
import os
import re
import stat
import sys

ID_RE = rb"^[a-z0-9]+(?:-[a-z0-9]+)*\n?$"
MAX_BYTES = 64
SELECTION_DIR = "/var/lib/desktop-theme"
SELECTION_NAME = "william"


def store_path(path):
    if not isinstance(path, str) or not path.startswith("/nix/store/"):
        return False
    if "\n" in path or "\0" in path:
        return False
    return ".." not in path.split("/")


def read_bounded(dir_path, name=SELECTION_NAME):
    """Return a theme ID, or None when the file is missing or unusable.

    Opens the parent with O_DIRECTORY|O_NOFOLLOW and the file with
    O_NOFOLLOW|O_NONBLOCK, then requires a regular file before reading.
    """
    try:
        dirfd = os.open(dir_path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return None
    try:
        try:
            fd = os.open(
                name,
                os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                dir_fd=dirfd,
            )
        except OSError:
            return None
        try:
            info = os.fstat(fd)
            if not stat.S_ISREG(info.st_mode):
                return None
            data = os.read(fd, MAX_BYTES + 1)
        finally:
            os.close(fd)
    finally:
        os.close(dirfd)
    if len(data) > MAX_BYTES or not _id_bytes(data):
        return None
    text = data.decode("ascii").rstrip("\n")
    if text == "" or "\n" in text:
        return None
    return text


def _id_bytes(data):
    return isinstance(data, bytes) and re.fullmatch(ID_RE, data) is not None


def resolve(theme_id, themes, fallback):
    entry = themes.get(theme_id) if theme_id else None
    if entry is None:
        if fallback not in themes:
            raise SystemExit("greeter fallback is not in the allowlist")
        entry = themes[fallback]
    for key in ("config", "css"):
        if not store_path(entry.get(key)):
            raise SystemExit("refusing non-store theme asset")
    return entry


def command_for(manifest, theme_id):
    for key in ("dbus", "cage", "regreet"):
        if not store_path(manifest[key]):
            raise SystemExit("refusing non-store greeter executable")
    entry = resolve(theme_id, manifest["themes"], manifest["fallback"])
    argv = [
        manifest["dbus"],
        manifest["cage"],
        *manifest["cageArgs"],
        "--",
        manifest["regreet"],
        "--config",
        entry["config"],
        "--style",
        entry["css"],
    ]
    current = os.environ.get("XDG_DATA_DIRS", "/run/current-system/sw/share")
    share = manifest["sessionShare"]
    env = os.environ.copy()
    if share and share not in current.split(":"):
        env["XDG_DATA_DIRS"] = share + ":" + current
    return argv, env


def launch(manifest_path):
    with open(manifest_path, encoding="utf-8") as handle:
        manifest = json.load(handle)
    theme_id = read_bounded(manifest["selectionDir"], manifest["selectionName"])
    argv, env = command_for(manifest, theme_id)
    os.execvpe(argv[0], argv, env)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest")
    args = parser.parse_args(argv)
    launch(args.manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
