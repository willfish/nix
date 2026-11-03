# Inactive worktree cleanup

`git cm` uses `wtclean` for primary checkouts directly under
`~/Repositories/hmrc` whose origin is a `trade-tariff/*` GitHub repository.
It switches to the default branch, fetches, fast-forwards, then cleans eligible
linked worktrees. Other repositories retain their existing cleanup behaviour.

Preview or clean every matching repository without switching branches:

```sh
wtclean --dry-run
wtclean
```

`wtclean` cleans immediately without confirmation; `--dry-run` only previews.
Normal output summarizes retained worktrees and names failed checks. Add
`--verbose` for individual retention reasons and isolated state paths.
Add `--repo PATH` to limit the operation to one primary checkout. Worktrees can
be anywhere, but their exact HEAD must match one same-repository merged or
closed PR, including stacked PRs, with no open PR on the branch. Squash
merges are supported. Lookup failures, changed PR tips, local changes,
untracked files, index-hidden changes and locked worktrees are retained.
Primary checkouts and local branches are never deleted. Missing registrations
are pruned using Git without deleting their unverified isolated state.

Cleanup verifies the flake's path-hashed worktree ID and state conventions,
stops isolated PostgreSQL using its original Nix executable, then removes the
checkout without forcing Git and deletes its isolated database, socket, PID,
gem, bundle and Yarn paths. Unknown conventions or a failed stop retain the
checkout and state. Orphaned state without a registered checkout is not swept.

Run `bash programs/wtclean/check.sh` from the dotfiles dev shell for an offline
check using disposable Git worktrees and PostgreSQL. It uses existing local
PostgreSQL binaries and never contacts GitHub.
