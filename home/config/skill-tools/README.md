# Native skill helpers

`contrast` checks an opaque six-digit sRGB pair against an unrounded threshold.
It does not establish accessibility or evaluate composited colours.

`youtube-extract` retains the metadata, caption cleanup, rolling deduplication,
time-window and casefolded search CLI. It prefers a host `yt-dlp`, falling back
to ephemeral Nix, and requests metadata and English subtitles without media,
tolerating partial caption failures so metadata and remaining subtitle
languages survive a 429. Local fixture inputs never fetch. Temporary downloads are removed; a supplied
work directory is retained. Third-party yt-dlp remains unchanged.

`qbittorrent-inventory` reads only fast-resume-declared payloads, preserves raw
NUL-delimited transfer names and rejects traversal or payload symlink escapes.
`source-target-duplicate-check` keeps exact-name, normalized-title, ASIN and size
hints. A missing, unreadable or broken target makes the entire report incomplete,
with no matches or staging eligibility. Directory symlinks are not traversed.
`libation-inventory` prints existing indexed media paths, never account data.
These helpers inventory only; they do not copy, delete or import media.

`pi-token-report` rebuilds private HTML/JSON token reports or orchestrates the
unchanged three-turn capture workload. Provider token totals and character-share
cost estimates remain distinct. Reports omit arbitrary arguments, private query
text, URL credentials and unexpected probe output. The third-party Python runtime
is retained solely to run the workload's model-generated `add.py`, not reporter
logic. Live capture still requires the skill's explicit user authorization.

Home Manager copies the host-native executables into their public skill bundles.
Private skill overrides, discovery metadata and authorization gates are unchanged.
From this checkout, use `nix shell .#skill-tools -c contrast ...` or
`nix shell .#skill-tools -c youtube-extract ...`.

## Manual verification

With Meson, Ninja, pkg-config, GLib/GIO and yyjson available:

```sh
meson setup /tmp/skill-tools-checks home/config/skill-tools -Dfixtures=true --buildtype=debugoptimized
meson compile -C /tmp/skill-tools-checks
/tmp/skill-tools-checks/skill-tools-checks
```

Fixtures are program-local, noninstalled and unregistered. Downloader tests use
stub executables, not live YouTube or browser sessions. Optional `CONTRAST_LEGACY`,
`YOUTUBE_LEGACY` and `LEGACY_PYTHON` enable comparisons with retained reference
scripts outside the repository. The audiobook suite also accepts
`INVENTORY_LEGACY` and `DUPLICATE_LEGACY`. A separate build can use
`-Db_sanitize=address,undefined -Dbuildtype=debugoptimized`.
