# Native skill helpers

`contrast` checks an opaque six-digit sRGB pair against an unrounded threshold.
It does not establish accessibility or evaluate composited colours.

`youtube-extract` retains the metadata, caption cleanup, rolling deduplication,
time-window and casefolded search CLI. It prefers a host `yt-dlp`, falling back
to ephemeral Nix, and requests metadata and English subtitles without media.
Local fixture inputs never fetch. Temporary downloads are removed; a supplied
work directory is retained. Third-party yt-dlp remains unchanged.

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
  node --test home/config/skill-tools/tests/tools.test.ts
```

Fixtures are program-local, noninstalled and unregistered. Downloader tests use
stub executables, not live YouTube or browser sessions. Optional `CONTRAST_LEGACY`,
`YOUTUBE_LEGACY` and `LEGACY_PYTHON` enable comparisons with retained reference
scripts outside the repository. A separate build can use
`-Db_sanitize=address,undefined -Dbuildtype=debugoptimized`.
