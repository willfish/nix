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

## Managed configuration

- `hermes-declaration DECLARATION HOME [--qwen-overlay FILE --key-file FILE]`
  validates allowed paths and payloads before publication. It waits up to thirty
  seconds for the scheduler lock, retains runtime job history only for unchanged
  schedules, preserves completed one-shots, seeds missing OAuth files, and backs
  up replaced/stale content by SHA-256 before removing or replacing it. Files,
  backups and markers remain private; executable assets use mode 0700.
- `hermes-profile PROFILE OVERLAY [--key-file FILE]` recursively merges settings
  and replaces/extends custom providers by name. It retains unrelated settings
  and credentials, takes a private timestamped backup before a changed write,
  refuses a symlinked profile, and leaves semantic no-ops untouched.
- `hermes-telegram-route CONFIG [--house FILE]` reconciles only the named
  `telegram-qwen` route. It requires a declared Qwen topic, mirrors changed routes
  into the gateway, removes the owned route when the topic disappears, and is
  quiet and idempotent.

YAML is parsed and emitted with libyaml, retaining nested mappings, scalar types,
merge keys and aliases without Python constructors. Changed YAML is normalized
and may use different quoting/tag layout; private backups preserve previous
content. This is semantic YAML compatibility, not byte-identical formatting.
JSON overlays and scheduler state use yyjson. Runtime keys are read or securely
created at activation time, never embedded in the Nix store. Partial publication
retains completed earlier writes and their backups, as before; it is not an
all-files transaction. Errors never print configuration or secret values.

## Manual verification

With GLib/GIO, yyjson, libyaml, Meson, Ninja, pkg-config, Node, SOPS, age and yq-go supplied by the
existing direnv/ephemeral Nix environment:

```sh
meson setup /tmp/hermes-tools-build home/config/hermes-tools
meson compile -C /tmp/hermes-tools-build
HERMES_EXPORT_BIN=/tmp/hermes-tools-build/hermes-export \
  node --experimental-strip-types --test home/config/hermes-tools/export.test.ts
HERMES_MANAGED_BIN_DIR=/tmp/hermes-tools-build \
  node --experimental-strip-types --test home/config/hermes-tools/managed.test.ts
```

The fixtures use disposable directories, a fake SOPS process for failure and
argument checks, and an ephemeral age key for a real encryption roundtrip.
Optional `HERMES_EXPORT_LEGACY` and `HERMES_EXPORT_PYTHON` compare decoded
capture results with the retained original. Fixtures are manual, uninstalled and
unregistered; package builds do not run them. Managed-helper comparisons use
`HERMES_MANAGED_LEGACY` for a directory of retained original modules and
`HERMES_MANAGED_PYTHON` for an interpreter with PyYAML. `HERMES_YQ` selects the
independent YAML decoder used by the fixtures. No test reads live Hermes state
or activates a remote host.
