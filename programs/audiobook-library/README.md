# Audiobook library inspection

This package contains related, read-only library preparation commands:

- `qbittorrent-inventory` reads fast-resume-declared payloads, preserves raw
  NUL-delimited names and rejects traversal or payload symlink escapes.
- `source-target-duplicate-check` reports exact-name, normalized-title, ASIN and
  size hints. A missing, unreadable or broken target makes the entire report
  incomplete, without matches or staging eligibility. Directory symlinks are
  not traversed.
- `libation-inventory` prints existing indexed media paths, never account data.

These commands do not copy, delete or import media. Build with
`nix build .#audiobook-library`. Manual fixtures use disposable libraries in the
[native skill command collection](../collections/skill-tools).
