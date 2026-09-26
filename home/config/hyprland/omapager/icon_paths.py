"""Icon path checks for Nix profile symlinks."""

import os
import stat


def _inside(path, root):
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:
        return False


def allowed_icon(path, bases):
    """Accept an icon found under a search directory.

    Nix profiles symlink icons into other store paths. A symlink that stays
    under the search directory may point at a regular file in /nix/store.
    Anything else outside the directory is rejected.
    """
    if not path or not os.path.isfile(path):
        return False
    logical = os.path.abspath(path)
    real = os.path.realpath(path)
    if not stat.S_ISREG(os.stat(real).st_mode):
        return False
    for base in bases:
        root = os.path.abspath(base)
        if not _inside(logical, root):
            continue
        if _inside(real, os.path.realpath(base)):
            return True
        if real.startswith("/nix/store/"):
            return True
    return False
