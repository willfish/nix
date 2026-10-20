#!/usr/bin/env bash
set -euo pipefail
binary="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=Example GIT_COMMITTER_NAME=Example
export GIT_AUTHOR_EMAIL=example@example.invalid
export GIT_COMMITTER_EMAIL=example@example.invalid
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE

git_cmd() {
  git -c core.hooksPath=/dev/null -c core.fsmonitor=false "$@"
}

mkdir "$scratch/repo"
cd "$scratch/repo"
git_cmd init -q -b main
"$binary" >"$scratch/empty"
grep -F 'main (no commits yet)' "$scratch/empty"
grep -F 'No local branches yet.' "$scratch/empty"
tree=$(git_cmd mktree </dev/null)
base=$(git_cmd commit-tree "$tree" -m 'Initial example')
local_tip=$(git_cmd commit-tree "$tree" -p "$base" -m 'Local example')
remote_tip=$(git_cmd commit-tree "$tree" -p "$base" -m 'Remote example')
git_cmd update-ref refs/heads/main "$base"
git_cmd config remote.origin.url "$scratch/offline"
git_cmd config remote.origin.fetch '+refs/heads/*:refs/remotes/origin/*'
for name in main ahead behind diverged gone; do
  tip=$base
  remote=$base
  case "$name" in ahead | diverged) tip=$local_tip ;; esac
  case "$name" in behind | diverged) remote=$remote_tip ;; esac
  git_cmd update-ref "refs/heads/$name" "$tip"
  if [[ $name != gone ]]; then
    git_cmd update-ref "refs/remotes/origin/$name" "$remote"
  fi
  git_cmd config "branch.$name.remote" origin
  git_cmd config "branch.$name.merge" "refs/heads/$name"
done
git_cmd branch no-upstream
git_cmd worktree add -q "$scratch/linked worktree" ahead
git_cmd symbolic-ref refs/remotes/origin/HEAD refs/remotes/origin/main
git_cmd show-ref >"$scratch/refs-before"
"$binary" --all >"$scratch/all"
for expected in \
  'in sync | origin/main' \
  'ahead 1 | origin/ahead' \
  'behind 1 | origin/behind' \
  'ahead 1, behind 1 | origin/diverged' \
  'upstream gone | origin/gone' \
  'no upstream' \
  'Remote branches (last fetched) (4)'; do
  grep -F "$expected" "$scratch/all"
done
awk 'NR == 3 { if ($1 != "main" || $2 != "current") exit 1 }' "$scratch/all"
grep -E '^ahead +worktree' "$scratch/all"
if grep -F 'origin/HEAD' "$scratch/all"; then exit 1; fi
if grep -F $'\033' "$scratch/all"; then exit 1; fi
"$binary" --remotes >"$scratch/remotes"
if grep -F 'Local branches' "$scratch/remotes"; then exit 1; fi
"$binary" >"$scratch/local"
if grep -F 'Remote branches' "$scratch/local"; then exit 1; fi
if "$binary" --all --remotes >"$scratch/error" 2>&1; then exit 1; fi
if "$binary" --color=invalid >"$scratch/error" 2>&1; then exit 1; fi
"$binary" --color always >"$scratch/color"
for style in '1;32' '36' '33' '1;31'; do
  grep -Fq $'\033['"${style}m" "$scratch/color"
done
"$binary" --color=never >"$scratch/plain"
if grep -Fq $'\033' "$scratch/plain"; then exit 1; fi
git_cmd show-ref >"$scratch/refs-after"
cmp "$scratch/refs-before" "$scratch/refs-after"
git_cmd checkout -q --detach "$base"
"$binary" >"$scratch/detached"
grep -F 'HEAD detached at' "$scratch/detached"
git_cmd symbolic-ref HEAD refs/heads/unborn
"$binary" >"$scratch/unborn"
grep -F 'unborn (no commits yet)' "$scratch/unborn"
cd "$scratch"
if "$binary" >"$scratch/error" 2>&1; then exit 1; fi
grep -F 'not a git repository' "$scratch/error"
"$binary" --help >/dev/null
cd repo
"$binary" --full >/dev/null
printf '%s\n' 'PASS: Git states, scopes, colour, unchanged refs and errors'
