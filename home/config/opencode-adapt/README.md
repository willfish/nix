# OpenCode Markdown adapter

`opencode-adapt-markdown --kind agent|command SOURCE DESTINATION` converts Pi
Markdown into the existing OpenCode agent/command format. Home Manager uses the
build-platform executable to prepare its immutable bundles; no interpreter or
adapter is launched by OpenCode at runtime.

## Maintained format

This is a deliberately simple text adapter, not a YAML parser:

- Recognize only the initial `---` line and the first exact LF-delimited closing
  marker after that opening. Missing/malformed frontmatter remains body text.
- Split header lines using Python's Unicode line boundaries. Ignore lines
  beginning with an ASCII space, split the rest on the first colon, strip Unicode
  end whitespace and retain the last duplicate field.
- Agents render `description`, `mode`, `model` in that order. An absent mode
  defaults to `subagent`; a present but empty mode stays omitted. Other Pi-only
  keys do not enter frontmatter.
- Agent `skills` becomes the existing body instruction, using bracket/space
  character-set stripping and comma splitting. An exact trimmed instruction
  substring anywhere in the body prevents duplication.
- Commands render `description`, `agent`, `model`, `subtask`, and replace every
  body `$@` with `$ARGUMENTS`. Header values remain literal.
- Omit empty fields, preserve body data and remove only leading LF characters at
  render time. Skill-note insertion additionally strips trailing Python whitespace.
  Preserve embedded NUL and Unicode; strict UTF-8 reads normalize CRLF/lone CR.

## Filesystem behavior

Create the destination before scanning. Convert only sorted direct `*.md`
children, including hidden names, with case-sensitive suffix matching. Sort
valid Unicode and raw-byte filenames as Python surrogateescape strings. Missing
or unscannable sources are empty globs, and enumeration errors discard partial
names before writing.

Read a whole file before opening its destination. Direct writes follow existing
leaf symlinks and preserve existing modes/inodes. A later file failure retains
completed earlier outputs; write/close failure can leave that output partial.
Repeated source trees overlay matching names without deleting unrelated files.
Inputs and outputs are trusted build paths, not an untrusted filesystem boundary.

## Manual fixtures

Supply GLib, Meson, Ninja, pkg-config and Node in an ephemeral Nix environment
from the direnv checkout:

```sh
meson setup /tmp/opencode-adapt-build home/config/opencode-adapt \
  -Dfixtures=true -Dbuildtype=debugoptimized
meson compile -C /tmp/opencode-adapt-build
OPENCODE_ADAPT_BIN=/tmp/opencode-adapt-build/opencode-adapt-markdown \
OPENCODE_ADAPT_FAILURES=/tmp/opencode-adapt-build/opencode-adapt-failures.so \
  node --experimental-strip-types --test \
    home/config/opencode-adapt/tests/adapter.test.ts
```

Fixtures default off, are not installed and have no automatic registration. They
use disposable files and failure interposition, not live OpenCode sessions.
Optional parity uses `OPENCODE_ADAPT_LEGACY` and `OPENCODE_ADAPT_PYTHON`.
`OPENCODE_ADAPT_PI_ROOT` and `OPENCODE_ADAPT_UPSTREAM` add full repository and
pinned upstream bundle comparisons. Without a legacy executable,
`OPENCODE_ADAPT_REFERENCE_BUNDLES` can point to JSON containing `agents` and
`commands` paths of retained legacy bundles, including for sanitizer runs.
For ASan/UBSan runs,
`OPENCODE_ADAPT_ASAN_RT` puts the compiler's runtime before the interposer.
Production recognizes none of those fixture controls.
