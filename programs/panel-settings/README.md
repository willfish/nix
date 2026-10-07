# Panel settings

Native C writers behind `hypr-calendar-settings JSON` and
`hypr-weather-settings JSON`. Home Manager supplies the fixed destination under
its configured XDG configuration home; the packaged commands take
`DESTINATION JSON`.

The commands retain the existing contract:

- Exactly one public JSON argument, bounded to 8,000 Unicode characters. Valid
  nonobjects, wrong argument counts and oversize payloads return 2; invalid JSON or publication failures
  return 1. Successful publication returns 0 and emits no output.
- ASCII JSON with the original spaces, insertion order and final LF. Duplicate
  keys retain their first position and last value. Preserve arbitrary integers,
  real-number spelling, NaN/infinities, escaped surrogates and embedded NUL data.
- Match filesystem-argument surrogateescape decoding. Character bounds do not
  become UTF-8-byte or UTF-16-code-unit bounds.
- Create a missing final parent with mode 0700 subject to umask. Intermediate
  directories use the ordinary mkdir default; existing permissions remain.
- Write a mode-0600 `.calendar-settings-`/`.weather-settings-` temporary sibling,
  complete short writes, close, then rename. Failures remove that temporary and
  retain the old destination. New parents survive later encoding/write failures.
- Keep existing parent-link and leaf-replacement behavior. Replacing a leaf link
  does not write its target. Do not change the QML watcher paths or host boundary.

The bounded native codec retains the former interpreter's JSON construction and
encoding behavior without a Python runtime, schema restriction or rounding/Unicode
changes introduced by a generic JSON library. It retains the decimal conversion
and streaming-encoder nesting limits as well as their publication ordering.

## Manual fixtures

Build with GLib, Meson, Ninja and pkg-config in an ephemeral Nix environment from
the direnv checkout:

```sh
meson setup /tmp/panel-settings-build programs/panel-settings \
  -Dfixtures=true -Dbuildtype=debugoptimized
meson compile -C /tmp/panel-settings-build
PANEL_SETTINGS_BIN_DIR=/tmp/panel-settings-build \
PANEL_SETTINGS_FAILURES=/tmp/panel-settings-build/panel-failures.so \
PANEL_SETTINGS_ARGV=/tmp/panel-settings-build/panel-argv \
  /tmp/panel-settings-build/panel-settings-check
```

Fixtures default off, are never installed and have no automatic registration.
They use disposable destinations, raw-argv construction and failure interposition
for write, chmod, close, rename and publication timing. Optional legacy parity
uses `PANEL_SETTINGS_LEGACY=/path/panel-{kind}-legacy.py` and
`PANEL_SETTINGS_PYTHON`. For ASan/UBSan runs, `PANEL_SETTINGS_ASAN_RT` places the
compiler's ASan runtime before the interposition library. Production commands
recognize none of these fixture controls.
