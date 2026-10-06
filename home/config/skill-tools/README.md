# Native skill helpers

`contrast` checks an opaque six-digit sRGB pair against an unrounded threshold.
It does not establish accessibility or evaluate composited colours.

`youtube-extract` retains the metadata, caption cleanup, rolling deduplication,
time-window and casefolded search CLI. It prefers a host `yt-dlp`, falling back
to ephemeral Nix, and requests metadata and English subtitles without media.
Local fixture inputs never fetch. Temporary downloads are removed; a supplied
work directory is retained. Third-party yt-dlp remains unchanged.

`qbittorrent-inventory` reads only fast-resume-declared payloads, preserves raw
NUL-delimited transfer names and rejects traversal or payload symlink escapes.
`source-target-duplicate-check` keeps exact-name, normalized-title, ASIN and size
hints. A missing, unreadable or broken target makes the entire report incomplete,
with no matches or staging eligibility. Directory symlinks are not traversed.
`libation-inventory` prints existing indexed media paths, never account data.
These helpers inventory only; they do not copy, delete or import media.

Home Manager copies the host-native executables into their public skill bundles.
Private skill overrides, discovery metadata and authorization gates are unchanged.
From this checkout, use `nix shell .#skill-tools -c contrast ...` or
`nix shell .#skill-tools -c youtube-extract ...`.

## Manual verification

With Meson, Ninja, pkg-config, GLib/GIO, yyjson and Node available:

```sh
meson setup /tmp/skill-tools-build home/config/skill-tools
meson compile -C /tmp/skill-tools-build
SKILL_TOOLS_BIN=/tmp/skill-tools-build \
  node --test home/config/skill-tools/tests/*.test.ts
```

Fixtures are program-local, noninstalled and unregistered. Downloader tests use
stub executables, not live YouTube or browser sessions. Optional `CONTRAST_LEGACY`,
`YOUTUBE_LEGACY` and `LEGACY_PYTHON` enable comparisons with retained reference
scripts outside the repository. The audiobook suite also accepts
`INVENTORY_LEGACY` and `DUPLICATE_LEGACY`. A separate build can use
`-Db_sanitize=address,undefined -Dbuildtype=debugoptimized`.
