# Merged worktree cleanup

`git cm` uses `wtclean` for primary checkouts directly under
`~/Repositories/hmrc` whose origin is a `trade-tariff/*` GitHub repository.
It switches to the default branch, fetches, fast-forwards, then cleans eligible
linked worktrees. Other repositories retain their existing cleanup behaviour.

Preview or clean every matching repository without switching branches:

```sh
wtclean --dry-run
wtclean --yes
```

Add `--repo PATH` to limit the operation to one primary checkout. Worktrees can
be anywhere, but their exact HEAD must match one same-repository merged PR
targeting the default branch. Squash merges are supported. Closed-but-unmerged
PRs, lookup failures, local changes, untracked files, index-hidden changes,
locked worktrees and missing checkouts are retained. Primary checkouts and
local branches are never deleted.

Cleanup verifies the flake's path-hashed worktree ID and state conventions,
stops isolated PostgreSQL using its original Nix executable, then removes the
checkout without forcing Git and deletes its isolated database, socket, PID,
gem, bundle and Yarn paths. Unknown conventions or a failed stop retain the
checkout and state. Orphaned state without a registered checkout is not swept.

Run `bash programs/wtclean/check.sh` from the dotfiles dev shell for an offline
check using disposable Git worktrees and PostgreSQL. It uses existing local
PostgreSQL binaries and never contacts GitHub.
