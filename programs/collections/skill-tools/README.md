# Native skill command collection

`skill-tools` is a compatibility package collecting
[color-contrast](../../color-contrast), [youtube-extract](../../youtube-extract),
[audiobook-library](../../audiobook-library) and
[pi-token-report](../../pi-token-report). Each program owns its implementation and
package definition. Existing `nix shell .#skill-tools` callers retain the same
commands.

Home Manager inserts the host-native commands into their public skill bundles.
Private overrides, discovery metadata and authorization gates are unchanged.

## Manual checks

The existing multi-command fixtures stay local to this collection. They are
noninstalled, default-off and unregistered with package builds, flake checks and
hooks. Downloader and capture checks use compiled stubs, synthetic traces and
disposable media directories, not live services, inference or transfers.

From the repository root:

```sh
package=$(direnv exec . nix build .#skill-tools --no-link --print-out-paths)
direnv exec . nix develop .#color-contrast -c meson setup \
  /tmp/skill-tools-checks programs/collections/skill-tools -Dfixtures=true
direnv exec . nix develop .#color-contrast -c meson compile -C /tmp/skill-tools-checks
SKILL_TOOLS_BIN="$package/bin" /tmp/skill-tools-checks/skill-tools-checks
```

Optional external references use `CONTRAST_LEGACY`, `YOUTUBE_LEGACY`,
`INVENTORY_LEGACY`, `DUPLICATE_LEGACY` and `LEGACY_PYTHON`. They are not package
inputs. Do not substitute real private captures or provider calls for fixtures.
