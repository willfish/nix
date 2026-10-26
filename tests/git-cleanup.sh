#!/usr/bin/env bash
# Manual offline check of the installed cleanup script. No network or real trees.
set -euo pipefail
cleanup="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export REAL_GIT
REAL_GIT=$(command -v git)
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=Example GIT_COMMITTER_NAME=Example
export GIT_AUTHOR_EMAIL=example@example.invalid
export GIT_COMMITTER_EMAIL=example@example.invalid
export CLEANUP_FIXTURES="$scratch/responses"
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
mkdir -p "$scratch/bin" "$CLEANUP_FIXTURES"
"$REAL_GIT" init -q --bare "$scratch/origin"
"$REAL_GIT" init -q -b main "$scratch/repo"
cd "$scratch/repo"
printf 'original\n' >tracked
"$REAL_GIT" add tracked
"$REAL_GIT" commit -qm initial
"$REAL_GIT" remote add origin "$scratch/origin"
"$REAL_GIT" push -qu origin main
"$REAL_GIT" symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main
printf '.envrc\ncache\n!/.envrc\n' >.git/info/exclude
base=$("$REAL_GIT" rev-parse HEAD)
tree=$("$REAL_GIT" rev-parse 'HEAD^{tree}')
cases=(closed changed open dirty deleted untracked envrc locked api-error status-error
  fork other-base full-page malformed wrong-name ambiguous)
mkdir -p "$scratch/repo/.worktrees"
for branch in "${cases[@]}"; do
  tip=$("$REAL_GIT" commit-tree "$tree" -p "$base" -m "Unmerged $branch")
  "$REAL_GIT" branch "$branch" "$tip"
  if [[ $branch == envrc ]]; then
    "$REAL_GIT" worktree add -q "$scratch/repo/.worktrees/envrc" "$branch"
  else
    "$REAL_GIT" worktree add -q "$scratch/$branch" "$branch"
  fi
  jq -n --arg branch "$branch" --arg tip "$tip" '[{
    number: 42, state: "CLOSED", headRefName: $branch, headRefOid: $tip,
    isCrossRepository: false, baseRefName: "main"
  }]' >"$CLEANUP_FIXTURES/$branch.json"
done
# A live upstream must not prevent the verified closed-PR path.
"$REAL_GIT" push -qu origin closed
printf 'generated\n' >"$scratch/closed/cache"
closed_tip=$("$REAL_GIT" rev-parse closed)
new_tip=$("$REAL_GIT" commit-tree "$tree" -p changed -m 'New local work')
"$REAL_GIT" update-ref refs/heads/changed "$new_tip"
printf 'changed\n' >"$scratch/dirty/tracked"
rm "$scratch/deleted/tracked"
printf 'untracked\n' >"$scratch/untracked/local-work"
printf 'local config\n' >"$scratch/repo/.worktrees/envrc/.envrc"
mkdir -p "$scratch/repo/.worktrees/envrc/nested"
printf 'nested config\n' >"$scratch/repo/.worktrees/envrc/nested/.envrc"
[[ $("$REAL_GIT" -C "$scratch/repo/.worktrees/envrc" status --porcelain --untracked-files=all -- .envrc) == '?? .envrc' ]]
[[ $("$REAL_GIT" -C "$scratch/repo/.worktrees/envrc" status --porcelain --untracked-files=all --ignored -- nested/.envrc) == '!! nested/.envrc' ]]
"$REAL_GIT" worktree lock "$scratch/locked"
for branch in open fork other-base full-page wrong-name ambiguous; do
  # jq, not the shell, expands $n in the full-page fixture.
  # shellcheck disable=SC2016
  case "$branch" in
  open) filter='. + [.[0] | .state = "OPEN"]' ;;
  fork) filter='.[0].isCrossRepository = true' ;;
  other-base) filter='.[0].baseRefName = "other"' ;;
  full-page) filter='[range(100) as $n | .[0] | .number = ($n + 1)]' ;;
  wrong-name) filter='.[0].headRefName = "other"' ;;
  ambiguous) filter='. + [.[0] | .number = 43]' ;;
  esac
  jq "$filter" "$CLEANUP_FIXTURES/$branch.json" >"$scratch/response"
  mv "$scratch/response" "$CLEANUP_FIXTURES/$branch.json"
done
"$REAL_GIT" worktree add -qb landed "$scratch/landed" main
printf 'landed config\n' >"$scratch/landed/.envrc"
[[ $("$REAL_GIT" -C "$scratch/landed" status --porcelain --untracked-files=all -- .envrc) == '?? .envrc' ]]

# Keep Git operations real, but make origin identity and GitHub replies offline.
cat >"$scratch/bin/git" <<'GIT'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$*" == 'remote get-url origin' ]]; then
  printf '%s\n' 'git@github.com:example/project.git'
  exit 0
fi
if [[ "${1:-}" == -C && "${2:-}" == */status-error && "${3:-}" == status ]]; then
  exit 128
fi
if [[ "${1:-}" == worktree && "${2:-}" == remove && "${3:-}" == --force ]]; then
  echo 'Unexpected forced removal' >&2
  exit 99
fi
exec "$REAL_GIT" "$@"
GIT
cat >"$scratch/bin/gh" <<'GH'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1 $2" == 'pr list' ]]
shift 2
branch=''
while (($#)); do
  case "$1" in
    --repo) [[ "$2" == example/project ]]; shift 2 ;;
    --head) branch=$2; shift 2 ;;
    --state) [[ "$2" == all ]]; shift 2 ;;
    --limit | --json) shift 2 ;;
    *) exit 2 ;;
  esac
done
case "$branch" in
  api-error) exit 1 ;;
  malformed) printf 'not JSON\n' ;;
  *) jq . "$CLEANUP_FIXTURES/$branch.json" ;;
esac
GH
chmod +x "$scratch/bin/git" "$scratch/bin/gh"
PATH="$scratch/bin:$PATH" "$cleanup"
[[ ! -d "$scratch/closed" ]]
[[ ! -d "$scratch/repo/.worktrees/envrc" ]]
[[ $("$REAL_GIT" rev-parse closed) == "$closed_tip" ]]
"$REAL_GIT" show-ref --verify --quiet refs/heads/envrc
[[ ! -d "$scratch/landed" ]]
if "$REAL_GIT" show-ref --verify --quiet refs/heads/landed; then exit 1; fi
for branch in "${cases[@]:1}"; do
  [[ $branch == envrc ]] && continue
  [[ -d "$scratch/$branch" ]]
  "$REAL_GIT" show-ref --verify --quiet "refs/heads/$branch"
done
[[ $("$REAL_GIT" -C "$scratch/dirty" diff --numstat) == $'1\t1\ttracked' ]]
printf '%s\n' 'PASS: closed and envrc checkouts removed, branches retained, unsafe cases kept, landed cleanup preserved'
