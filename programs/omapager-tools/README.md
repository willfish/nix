# Omapager tools

Owned C adapters for the pinned upstream desktop notification shell:

- `omapager-prepare-shell OUTPUT`: adapt Omarchy's Color and KeyboardPanel QML.
- `omapager-patch-icon ICON_COMMAND`: add installed-name alias/token lookups.
- `omapager-patch-herdr-focus SERVICE_QML FOCUS_BIN`: add Herdr toast actions.
- `icon_paths.so`: compiled `expand_names`, `name_matches` and `allowed_icon`
  functions imported by upstream's Python icon command.

Patchers replace the first matching anchor and fail if an anchor is absent. They
retain UTF-8/universal-newline handling, direct writes and preparation ordering:
Color commits before KeyboardPanel is read. Changes within each individual file
are prepared before its write. Upstream patch text is data, not an owned Python
command.

The icon module preserves alias order, Unicode character bounds and exact token
matching. A discovered regular file must lie logically under its search root;
its real target must stay under that root or lie in `/nix/store/`. The store
exception does not authorize unrelated store paths. CPython's native Unicode
and filesystem primitives retain bytes, lexical paths and exception semantics.

Upstream Python/Pillow and its helper policy, environment, resource limits and
network authorization remain unchanged. The module uses the same Python ABI as
that interpreter. Home Manager selects build-platform patch commands and copies
the host-platform module into the trusted upstream helper directory.

## Manual fixtures

Use the checkout's direnv environment and an ephemeral Nix shell supplying
Meson, Ninja, pkg-config, GLib and the flake-pinned Python development headers.
No temporary tooling belongs in project manifests.

```sh
nix develop .#omapager-tools --command env NIX_HARDENING_ENABLE= \
  meson setup /tmp/omapager-checks programs/omapager-tools \
  -Dfixtures=true --buildtype=debugoptimized
nix develop .#omapager-tools --command meson compile -C /tmp/omapager-checks
OMAPAGER_PLUGIN_SOURCE=/path/to/pinned/omapager \
OMAPAGER_OMARCHY_SOURCE=/path/to/pinned/omarchy \
  /tmp/omapager-checks/omapager-checks
```

`OMAPAGER_TOOLS_DIR`, `OMAPAGER_ICONS_DIR` and `OMAPAGER_ICON_FIXTURE` still
select those paths; otherwise the checks use the build directory and its
`icon-fixture`. Configure once without `-Dfixtures=true` to confirm the package
build does not create the check executable.

`OMAPAGER_ICONS_DIR` may select a packaged module separately from the command
folder. `OMAPAGER_LEGACY_DIR` enables optional parity against retained old sources
outside the repository. Set `OMAPAGER_RUNTIME_BIN` to a built bundle's helper
folder and `OMAPAGER_PYTHON` to its interpreter to exercise the actual isolated
upstream worker with disposable local icons and no fetching. This integration
case is explicitly skipped without the bundle.

Fixtures default off, are not installed and have no automatic test registration.
`-Dicons=false` builds only the patch/preparation commands, without CPython.
Sanitizer builds of the module run inside the sanitized embedding fixture;
upstream worker integration uses the normal bundle so its address-space limit
remains intact. CPython interpreter allocations can require LeakSanitizer
exclusion; do not treat that as ownership-leak coverage.
