# Hermes configuration tools

`hermes-export HOME ENCRYPTED_OUTPUT` captures a private Hermes declaration and
passes it directly to SOPS on stdin. The output repository supplies its recipient
policy. The command is installed on Relay; it is also available through
`nix run .#hermes-tools -- HOME ENCRYPTED_OUTPUT` on Linux.

The exporter retains configuration and executable assets, not sessions or
scheduler history. Existing Home Manager leaf symlinks remain owned elsewhere.
Hidden/cache directories and known managed scripts are excluded; the legacy
mem0 plugin is relocated with first-owner deduplication. OAuth files are separate
seeds, and repeat counters restart at zero. Non-Markdown store references fail
rather than capturing ephemeral paths.

SOPS receives the destination filename override and runs from its resolved
repository directory. Plaintext is never written to a temporary declaration.
Child diagnostics are captured, not echoed. Encryption failure leaves existing
output untouched; successful ciphertext is written using the destination's
existing file/symlink semantics. Exporting real state is a separate deliberate
operation, not part of Home Manager activation or these fixtures.

## Manual verification

With GLib/GIO, yyjson, Meson, Ninja, pkg-config, Node, SOPS and age supplied by the
existing direnv/ephemeral Nix environment:

```sh
meson setup /tmp/hermes-tools-build home/config/hermes-tools
meson compile -C /tmp/hermes-tools-build
HERMES_EXPORT_BIN=/tmp/hermes-tools-build/hermes-export \
  node --experimental-strip-types --test home/config/hermes-tools/export.test.ts
```

The fixtures use disposable directories, a fake SOPS process for failure and
argument checks, and an ephemeral age key for a real encryption roundtrip.
Optional `HERMES_EXPORT_LEGACY` and `HERMES_EXPORT_PYTHON` compare decoded
capture results with the retained original. Fixtures are manual, uninstalled and
unregistered; package builds do not run them. The remaining declaration/profile/
routing helpers are separate from this exporter.
