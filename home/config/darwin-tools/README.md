# Darwin operator tools

C implementations of `darwin-health`, `darwin-preflight` and `darwin-deploy`.
Nix wrappers supply generated policy and health configurations. Deployment keeps
its fixed system profile, active-system and recovery paths, requires root and an
already-built store target, and never fetches inputs or bypasses preflight.

- Health persists only selected launchd state, numeric metrics and readiness,
  never raw command output. Recording requires root. Private atomic snapshots and
  copytruncate retain two bounded log tails without replacing the live inode.
  Concurrent writes during truncation can be lost; no service is signalled.
- Preflight checks platform, account, legacy jobs, runtime files, private-key
  metadata and secret ownership without adopting or stopping anything. Sorted
  policy diffs omit unknown installed fields. Unified hunk matching uses libgit2,
  so hunk grouping can differ from Python's `difflib` without changing the diff's
  metadata content.
- Deployment runs preflight before creating state and again under a nonblocking
  lock. Private recovery references precede profile registration. Failure restores
  only a still-owned profile pointer, not services or the active system. See the
  [Darwin recovery runbook](../../../docs/headless-darwin.md#recovery).

## Manual fixtures

These are opt-in, noninstalled and unregistered. They use disposable directories,
command responses and account/platform identities, not live launchd or sudo.
Run as an unprivileged user with Meson, Ninja, pkg-config, GLib/GIO, yyjson
and libgit2 available:

```sh
meson setup /tmp/darwin-tools-build home/config/darwin-tools -Dfixtures=true
meson compile -C /tmp/darwin-tools-build
/tmp/darwin-tools-build/darwin-checks
```

Use `-Db_sanitize=address,undefined -Dbuildtype=debugoptimized` for a separate
sanitized build. Linux fixtures and Darwin Nix evaluation do not establish native
macOS execution, service recovery or a paired system/home deployment. Those
operations require the runbook's deployment authorization.
