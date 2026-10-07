# Repository command collection

`repo-tools` is a compatibility package collecting independently named programs:
[check-flake-lock-update](../../check-flake-lock-update),
[import-omarchy-community](../../import-omarchy-community),
[audit-skills](../../audit-skills) and
[nix-storage-report](../../nix-storage-report). Their implementations and Nix
packages live in those program roots. Existing `nix shell .#repo-tools` callers
continue to receive all four commands.

## Manual checks

The existing multi-command fixtures stay local to this collection. They are
noninstalled, default-off and not registered with Meson tests, package phases,
flake checks or hooks. They use disposable files and stub Nix responses, never
live imports or merges.

From the repository root:

```sh
package=$(direnv exec . nix build .#repo-tools --no-link --print-out-paths)
direnv exec . nix develop .#check-flake-lock-update -c meson setup \
  /tmp/repo-tools-checks programs/collections/repo-tools -Dfixtures=true
direnv exec . nix develop .#check-flake-lock-update -c meson compile -C /tmp/repo-tools-checks
LOCK_POLICY_BIN="$package/bin/check-flake-lock-update" \
COMMUNITY_IMPORT_BIN="$package/bin/import-omarchy-community" \
SKILL_AUDIT_BIN="$package/bin/audit-skills" \
STORAGE_REPORT_BIN="$package/bin/nix-storage-report" \
  /tmp/repo-tools-checks/repo-checks
```

Optional legacy comparisons use external reference executables; they are not
package dependencies. `COMMUNITY_IMPORT_PAGE` supplies a reviewed local HTML
fixture. `SKILL_AUDIT_REFERENCE` accepts an external audit reference adapter.
