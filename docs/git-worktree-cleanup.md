# Git worktree cleanup

`git cm` moves to the origin default-branch worktree and runs `git-cleanup`.
Cleanup fetches and prunes origin, fast-forwards the current branch, and checks
other worktrees. It does not delete the current checkout or locked worktrees.

## Closed without merging

A clean worktree can be removed when GitHub confirms one closed, unmerged PR
whose head commit exactly matches the checkout. The PR must use the same
repository and branch, target the origin default branch, and have no open PR
on that branch. A live remote branch does not block this verified case.

The local branch and its commits remain. Cleanup does not delete remote branches.
New local commits, tracked changes and other untracked files prevent removal.
Ignored `.envrc` files do not block cleanup. Cleanup removes untracked `.envrc`
files from eligible worktrees before removing them, regardless of worktree
location. This path never uses forced removal.

The lookup supports GitHub origins using HTTPS or SSH. It uses the installed
`gh` and `jq` commands and existing GitHub access. Missing tools, failed lookups,
ambiguous results or a full result page retain the worktree. Cleanup does not
start a login. Non-GitHub repositories retain the existing landed-history rule.

Skipped candidates print a reason. Closed-PR removal prints the PR number and
confirms that the branch remains. Landed-history cleanup retains its existing
commit-subject matching and branch deletion rules.

## Offline check

Run the fixture against a built or installed cleanup command:

```sh
bash tests/git-cleanup.sh "$(command -v git-cleanup)"
```

The fixture uses temporary repositories and synthetic GitHub replies. It checks
retained commits, live upstreams, reused branches, changed tips, local dirt,
locks, lookup failures and the existing landed-history cleanup. It makes no
network requests and removes no real worktrees.
