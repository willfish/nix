# Pi configuration helpers

C commands used by Home Manager after secret activation. They keep Pi's auth,
settings and keybindings writable rather than replacing user state with store
symlinks. They do not log configuration values, secrets or credential paths.

```text
pi-merge-settings DEFAULTS SETTINGS
pi-merge-auth AUTH [--drop PROVIDER]...
  [--oauth-provider PROVIDER --refresh-file FILE --account-file FILE]
```

## Merge rules

Settings fill only missing top-level keys. Existing nested objects, null, false
and unknown fields remain user-owned. The exact former xAI Grok 4.6/4.7 and
OpenCode Go DeepSeek 4.1 Flash/GLM 5.3/Space Bunny defaults retarget to the declared
provider/model pair, even when the declaration omits one of those fields.

When declared `observational-memory-jev.enabledByDefault` is boolean false and
`.om-default-off-migrated` is absent, change an existing boolean true to false.
Write the marker only after successful settings publication. Marker-only runs
leave settings untouched; an existing marker preserves later user choices.

Auth removes only the explicitly named providers, then optionally seeds the
selected provider. Read both secret files only when both exist, before loading
auth, and strip Unicode end whitespace. Missing/empty pairs do not suppress
removals. Retain every existing entry with `type: "oauth"`, including expired or
incomplete entries. New entries contain `type`, `refresh`, `accountId`, empty
`access`, and zero `expires` in that order.

## Files and JSON

- A no-op preserves bytes, inode, mode and absent parent directories.
- Changed files use the existing `.tmp` sibling and atomic replacement, with
  final mode 0600. Staged files are also made private **before** writing any
  credentials. Failures retain the old destination and may retain the temporary;
  marker failure does not roll back already published settings.
- Preserve ordinary parent/leaf symlink behavior and lexical Pathlib-style path
  normalization. These commands operate on trusted user configuration paths,
  not an untrusted shared-directory filesystem boundary.
- Retain strict UTF-8 reads, universal newlines, object-only roots, ordered
  duplicate keys, arbitrary integers, nonfinite constants, escaped surrogates,
  NUL string data, ASCII escapes and indent-two JSON with a final LF. The local
  codec avoids generic JSON-library coercion of existing user values.
- Defensive decoding rejects more than 8,192 containers rather than exhausting
  the C stack. This is a native safety ceiling, not a promise to reproduce the
  interpreter's platform-dependent recursion cutoff.
- Application failures return 1 with a value-free diagnostic; usage errors
  return 2. Successful and no-op merges return 0 without output.

## Capture bus-host parsing

`pi-capture-bus-host` reads `PI_AGENT_BUS_URL` and prints an accepted hostname
plus LF, or just LF. Unset/empty values use `http://terminus:7420`. Accept only
lowercase HTTP(S) schemes, ASCII DNS labels and canonical dotted IPv4 when the
final label looks numeric or hexadecimal. Lowercase the output hostname, not
the caller's URL. Ports retain the existing decimal grammar without an added
range limit; paths reject Python whitespace, backslashes, queries and fragments.
This is the capture bypass policy, not a general URL validator.

The Pi wrapper combines both existing proxy-exclusion lists with the accepted
host. Unsupported URLs or parser failures disable bus participation for capture
without aborting Pi or rewriting the URL. Capture-off, offline, disabled-bus,
explicit credentials and missing-secret behavior remain unchanged.

## Manual fixtures

Use GLib, Meson, Ninja and pkg-config from an ephemeral Nix shell in the direnv
checkout:

```sh
meson setup /tmp/pi-config-build home/config/pi-config \
  -Dfixtures=true -Dbuildtype=debugoptimized
meson compile -C /tmp/pi-config-build
PI_CONFIG_BIN_DIR=/tmp/pi-config-build \
PI_CONFIG_FAILURES=/tmp/pi-config-build/pi-config-failures.so \
  node --experimental-strip-types --test home/config/pi-config/tests/merge.test.ts
PI_BUS_HOST_BIN=/tmp/pi-config-build/pi-capture-bus-host \
PI_BUS_HOST_ARGV=/tmp/pi-config-build/pi-host-argv \
  node --experimental-strip-types --test home/config/pi-config/tests/bus-host.test.ts
```

Fixtures default off, are not installed and have no automated registration.
They use disposable secrets, failed publication, no-op metadata checks and marker
ordering. Optional legacy comparison uses
`PI_CONFIG_LEGACY=/path/pi-merge-{kind}-legacy.py` and `PI_CONFIG_PYTHON`.
For sanitizer runs, `PI_CONFIG_ASAN_RT` places the compiler's ASan runtime before
the failure interposer. Host-parser parity uses `PI_BUS_HOST_LEGACY` and
`PI_BUS_HOST_PYTHON`. `PI_BUS_HOST_WRAPPERS` may supply JSON containing rendered
`foundation` and `public` Pi wrapper text; the fixtures replace every credential,
Pi and capture executable with disposable stubs before running it. They never
start real prompt capture, contact a provider/bus or read real secrets.
Production recognizes none of the fixture controls.
