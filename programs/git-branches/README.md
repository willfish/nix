# Git branch overview

`git branches` lists local branches with the current branch first, worktree use,
upstream sync, tip commit age and commit subject. `--all` includes remote-tracking
branches; `--remotes` lists only those branches. Symbolic refs such as
`origin/HEAD` are not separate branches.

Wide terminals get a table. Narrow terminals get wrapped branch names and sync
information. `--full` and redirected output include upstream names and retain
all columns without clipping.
Colour is optional: use `NO_COLOR=1` or `--color=never`. State labels do not rely
on colour. `git branch` remains unchanged.

The command only reads local Git metadata. It never fetches, checks out branches,
scans worktree dirtiness or calls a hosting service. Sync uses stored upstream
refs; commit age does not indicate recent checkout or use.

## Implementation and checks

The C17 program uses GLib/GIO for subprocesses, UTF-8 and allocation. GLib exits
on allocation failure, which is appropriate for this short-lived command.
Nix supplies Git through the executable wrapper. Output errors are checked at
final flush; a closed pipe uses the normal process SIGPIPE behaviour.

Run the offline fixture with a built executable:

```sh
bash programs/git-branches/check.sh /path/to/git-branches
```

For a manual sanitizer build in an environment with Meson, Ninja and GLib:

```sh
meson setup /tmp/git-branches-build programs/git-branches \
  --buildtype=debugoptimized -Db_sanitize=address,undefined
meson compile -C /tmp/git-branches-build
bash programs/git-branches/check.sh /tmp/git-branches-build/git-branches
```

The fixture uses temporary repositories and no provider or network access. It
is not installed or registered as a package build check. Terminal checks on Linux
can use `script` with `COLUMNS=40`, `80` and `160`; check colour-free output too.
