"""Icon path checks and app-name aliases for Nix profile symlinks."""

import os
import re
import stat

# Names the notification actually carries, mapped to icons this machine has.
_ALIASES = {
    "discord": ("discord",),
    "github": ("github",),
    "github-notifications": ("github",),
    "org.telegram.desktop": ("org.telegram.desktop",),
    "telegram": ("org.telegram.desktop", "telegram"),
    "telegram-desktop": ("org.telegram.desktop", "telegram"),
    "telegramdesktop": ("org.telegram.desktop", "telegram"),
    "whatsapp": ("whatsapp",),
    "whats-app": ("whatsapp",),
}


def _slug(text):
    slug = re.sub(r"[^a-z0-9._-]+", "-", str(text or "")[:256].lower())
    return slug.strip("-.")[:100]


def expand_names(names):
    """Add installed icon names for Discord, Telegram, WhatsApp and GitHub."""
    out = []
    seen = set()
    for name in names or []:
        raw = str(name or "").strip()
        if not raw:
            continue
        slug = _slug(raw)
        compact = slug.replace("-", "").replace(".", "")
        extras = []
        for key in (raw, raw.lower(), slug, compact):
            extras.extend(_ALIASES.get(key, ()))
        for candidate in (raw, slug, *extras):
            if candidate and candidate not in seen:
                seen.add(candidate)
                out.append(candidate)
    return out


def name_matches(want, haystack):
    """Match a desktop-entry token without treating dots as part of the word.

    The upstream check splits only on hyphens, so Name=Telegram never matches
    org.telegram.desktop and Discord's unhyphenated entry never matches either.
    """
    token = _slug(want)
    folded = token.replace("-", "").replace(".", "")
    if len(folded) < 3:
        return False
    pieces = [
        piece
        for piece in re.split(r"[\s._-]+", str(haystack or "").lower())
        if piece
    ]
    if token in pieces or folded in pieces:
        return True
    parts = [part for part in token.split("-") if part]
    if len(parts) < 2:
        return False
    width = len(parts)
    return any(
        pieces[i : i + width] == parts for i in range(len(pieces) - width + 1)
    )


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
