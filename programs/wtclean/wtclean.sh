#!/usr/bin/env bash
set -euo pipefail

dry_run=0 yes=0 check_only=0 repo_arg=''
while (($#)); do
  case "$1" in
  --dry-run)
    dry_run=1
    shift
    ;;
  --yes)
    yes=1
    shift
    ;;
  --repo | --check-repo)
    [[ $1 != --check-repo ]] || check_only=1
    [[ $# -ge 2 ]] || exit 2
    repo_arg=$2
    shift 2
    ;;
  -h | --help)
    echo 'usage: wtclean [--dry-run] [--yes] [--repo PATH]'
    echo 'Remove clean linked worktrees of HMRC trade-tariff GitHub repos with a merged PR at their exact HEAD.'
    echo 'Also stop and remove their isolated flake Postgres and caches. Keep primary checkouts and branches.'
    echo 'Default: preview, then require DELETE. --dry-run changes nothing. --yes skips confirmation.'
    exit 0
    ;;
  *)
    echo "Unknown option: $1" >&2
    exit 2
    ;;
  esac
done

home=$(cd "$HOME" && pwd -P)
cwd=$(pwd -P)
errors=0 removed=0
declare -a plan_repo=() plan_path=() plan_rev=() plan_branch=() plan_remote=() plan_default=() plan_pr=() plan_state=()
declare -a paths=() revs=() branches=() blocked=() state=()

git_at() { command git -c core.fsmonitor=false -C "$repo" "$@"; }
keep() { echo "KEEP $wt: $*"; }

repo_identity() {
  [[ ${repo%/*} == "$home/Repositories/hmrc" && -d "$repo/.git" && ! -L $repo ]] || return 1
  local origin
  origin=$(git_at remote get-url origin) || return 1
  case "$origin" in
  git@github.com:*) remote=${origin#git@github.com:} ;;
  https://github.com/*) remote=${origin#https://github.com/} ;;
  ssh://git@github.com/*) remote=${origin#ssh://git@github.com/} ;;
  *) return 1 ;;
  esac
  remote=${remote%.git}
  [[ $remote =~ ^trade-tariff/[a-zA-Z0-9_.-]+$ ]]
}

if [[ $check_only == 1 ]]; then
  repo=$(realpath -e "$repo_arg")
  repo_identity
  exit
fi

load_worktrees() {
  local listing field path='' rev='' branch='' block=0
  listing=$(mktemp) || return 1
  if ! git_at worktree list --porcelain -z >"$listing"; then
    rm -f -- "$listing"
    return 1
  fi
  paths=() revs=() branches=() blocked=()
  while IFS= read -r -d '' field; do
    case "$field" in
    worktree\ *) path=${field#worktree } ;;
    HEAD\ *) rev=${field#HEAD } ;;
    branch\ *) branch=${field#branch } ;;
    locked | locked\ * | prunable | prunable\ *) block=1 ;;
    '')
      if [[ -n $path ]]; then
        paths+=("$path") revs+=("$rev") branches+=("$branch") blocked+=("$block")
      fi
      path='' rev='' branch='' block=0
      ;;
    esac
  done <"$listing"
  rm -f -- "$listing"
}

checkout_safe() {
  local i=$1 other flags dirt actual common expected ident other_id count=0
  wt=${paths[$i]}
  [[ ${blocked[$i]} == 0 && -d $wt && ! -L $wt ]] || {
    keep 'locked, missing or symlinked'
    return 1
  }
  [[ $wt != "$repo" && "$cwd/" != "$wt/"* && ${branches[$i]} != "refs/heads/$default" ]] || {
    keep 'primary, current or default-branch checkout'
    return 1
  }
  [[ $wt != *$'\n'* ]] || {
    keep 'unsupported newline in path'
    return 1
  }
  [[ $(realpath -e "$wt") == "$wt" ]] || {
    keep 'symlink in worktree path'
    return 1
  }
  ident=$(printf '%s\n' "$wt" | md5sum | cut -c1-8) || return 1
  for other in "${paths[@]}"; do
    [[ $other != "$wt/"* ]] || {
      keep 'contains another registered worktree'
      return 1
    }
    other_id=$(printf '%s\n' "$other" | md5sum | cut -c1-8) || return 1
    if [[ $other_id == "$ident" ]]; then count=$((count + 1)); fi
  done
  [[ $count == 1 ]] || {
    keep 'worktree ID collision'
    return 1
  }
  flags=$(git_at -C "$wt" ls-files -v) || return 1
  if grep -q '^[a-zS] ' <<<"$flags"; then
    keep 'assume-unchanged or skip-worktree index entries'
    return 1
  fi
  dirt=$(git_at -C "$wt" status --porcelain --untracked-files=all --ignore-submodules=none) || return 1
  [[ -z $dirt ]] || {
    keep 'local changes or untracked files'
    return 1
  }
  actual=$(git_at -C "$wt" rev-parse HEAD) || return 1
  [[ $actual == "${revs[$i]}" ]] || {
    keep 'HEAD changed'
    return 1
  }
  actual=$(git_at -C "$wt" symbolic-ref -q HEAD || true)
  [[ $actual == "${branches[$i]}" ]] || {
    keep 'branch changed'
    return 1
  }
  common=$(git_at -C "$wt" rev-parse --path-format=absolute --git-common-dir) || return 1
  expected=$(git_at rev-parse --path-format=absolute --git-common-dir) || return 1
  [[ $common == "$expected" ]] || {
    keep 'different repository'
    return 1
  }
}

merged_pr() {
  local prs
  prs=$(gh api "repos/$remote/commits/$rev/pulls?per_page=100") || return 1
  jq -er --arg rev "$rev" --arg remote "$remote" --arg base "$default" '
    select(type == "array" and length < 100)
    | select(all(.[]; .state != "open"))
    | map(select((.merged_at | type == "string" and length > 0) and .state == "closed" and .head.sha == $rev
      and .head.repo.full_name == $remote and .base.repo.full_name == $remote and .base.ref == $base))
    | select(length == 1) | .[0].number
    | select(type == "number" and . > 0 and floor == .)
  ' <<<"$prs"
}

safe_state_path() {
  local p=$1 root
  case "$p" in
  "$home/"*) root=$home ;;
  /tmp/pg-*) root=/tmp ;;
  *) return 1 ;;
  esac
  while [[ $p != "$root" ]]; do
    [[ ! -L $p ]] || return 1
    p=${p%/*}
    [[ -n $p ]] || return 1
  done
  [[ ! -L $root && -O $1 ]]
}

collect_state() {
  local p suffix convention owner
  ident=$(printf '%s\n' "$wt" | md5sum | cut -c1-8) || return 1
  [[ $ident =~ ^[a-f0-9]{8}$ ]] || return 1
  state=()
  for suffix in .local/share/postgres/worktrees .local/share/gem/worktrees .cache/bundle/worktrees .local/share/yarn/worktrees; do
    p="$home/$suffix/$ident"
    [[ -e $p || -L $p ]] && state+=("$p")
  done
  for p in "/tmp/pg-$ident" "/tmp/pg-$ident.pid"; do
    [[ -e $p || -L $p ]] && state+=("$p")
  done
  [[ ${#state[@]} -gt 0 ]] || return 0
  grep -Eq -- '--show-toplevel[[:space:]]*\|[[:space:]]*md5sum[[:space:]]*\|[[:space:]]*cut -c1-8' "$wt/flake.nix" || {
    keep 'unknown flake worktree ID convention'
    return 1
  }
  for p in "${state[@]}"; do
    safe_state_path "$p" || {
      keep "symlinked state: $p"
      return 1
    }
    owner=$(stat -c %u -- "$p") || return 1
    [[ $owner == "$(id -u)" ]] || {
      keep "state owned by another user: $p"
      return 1
    }
    if [[ $p == "$home/"* ]]; then
      suffix=${p#"$home/"}
      convention="\$HOME/${suffix%/*}/\$WT_ID"
    else
      # Match the literal variable in the flake, not its shell expansion.
      # shellcheck disable=SC2016
      convention='/tmp/pg-$WT_ID'
    fi
    grep -qF -- "$convention" "$wt/flake.nix" || {
      keep "unknown flake state convention: $p"
      return 1
    }
  done
}

stop_postgres() {
  local data="$home/.local/share/postgres/worktrees/$ident" exe rest ctl recorded status
  if [[ ! -d $data ]]; then
    [[ ! -e /tmp/pg-$ident.pid && ! -d /tmp/pg-$ident ]] || {
      keep 'Postgres socket/PID exists without its data directory'
      return 1
    }
    return 0
  fi
  if [[ ! -f "$data/postmaster.opts" ]]; then
    [[ ! -e "$data/postmaster.pid" && ! -e /tmp/pg-$ident.pid ]] || {
      keep 'Postgres executable unknown'
      return 1
    }
    return 0
  fi
  read -r exe rest <"$data/postmaster.opts" || return 1
  exe=${exe#\"} exe=${exe%\"}
  [[ $exe =~ ^/nix/store/[a-zA-Z0-9.+_-]+/bin/postgres$ ]] || {
    keep 'unknown Postgres executable'
    return 1
  }
  ctl=${exe%/*}/pg_ctl
  [[ -x $ctl ]] || {
    keep 'original Nix pg_ctl is unavailable'
    return 1
  }
  if [[ -f "$data/postmaster.pid" ]]; then
    recorded=$(sed -n '2p' "$data/postmaster.pid") || return 1
    [[ $recorded == "$data" ]] || {
      keep 'Postgres PID names another database'
      return 1
    }
  fi
  status=0
  "$ctl" status -D "$data" >/dev/null 2>&1 || status=$?
  case "$status" in
  0) "$ctl" stop -D "$data" -m fast -w -t 30 || return 1 ;;
  3) ;;
  *)
    keep 'cannot establish Postgres status'
    return 1
    ;;
  esac
  [[ ! -f "$data/postmaster.pid" ]] || {
    keep 'Postgres PID remains'
    return 1
  }
}

shopt -s nullglob
repos=("$home"/Repositories/hmrc/*)
if [[ -n $repo_arg ]]; then repos=("$(realpath -e "$repo_arg")"); fi
for repo in "${repos[@]}"; do
  if ! repo_identity; then
    if [[ -n $repo_arg ]]; then
      echo "Not a primary HMRC trade-tariff checkout: $repo" >&2
      exit 1
    fi
    continue
  fi
  load_worktrees || {
    errors=$((errors + 1))
    continue
  }
  [[ ${#paths[@]} -gt 1 ]] || continue
  default=$(gh api "repos/$remote" --jq .default_branch) || {
    errors=$((errors + 1))
    continue
  }
  [[ -n $default && $default != null ]] || {
    errors=$((errors + 1))
    continue
  }
  for ((i = 1; i < ${#paths[@]}; i++)); do
    checkout_safe "$i" || continue
    rev=${revs[$i]}
    if ! pr=$(merged_pr); then
      keep 'no verified merged PR at this exact HEAD targeting the default branch'
      continue
    fi
    collect_state || continue
    echo "REMOVE $wt (merged PR #$pr)"
    for p in "${state[@]}"; do echo "  state: $p"; done
    plan_repo+=("$repo") plan_path+=("$wt") plan_rev+=("$rev") plan_branch+=("${branches[$i]}")
    plan_remote+=("$remote") plan_default+=("$default") plan_pr+=("$pr")
    snapshot=$(printf '%s\n' "${state[@]}")
    plan_state+=("$snapshot")
  done
done

echo "${#plan_path[@]} worktree(s) eligible. Primary checkouts and branches stay intact."
if [[ $dry_run == 1 || ${#plan_path[@]} == 0 ]]; then
  [[ $errors == 0 ]]
  exit
fi
if [[ $yes == 0 ]]; then
  [[ -t 0 ]] || {
    echo 'No terminal. Use --dry-run or explicitly approve with --yes.' >&2
    exit 2
  }
  read -r -p 'Delete these worktrees and isolated state? Type DELETE: ' answer
  [[ $answer == DELETE ]] || {
    echo 'Cancelled.'
    exit 0
  }
fi

apply_candidate() {
  local n=$1 i found=-1 pr_now snapshot p
  repo=${plan_repo[$n]} wt=${plan_path[$n]} rev=${plan_rev[$n]}
  remote=${plan_remote[$n]} default=${plan_default[$n]}
  load_worktrees || return 1
  for ((i = 1; i < ${#paths[@]}; i++)); do
    [[ ${paths[$i]} != "$wt" ]] || {
      found=$i
      break
    }
  done
  [[ $found != -1 && ${revs[$found]} == "$rev" && ${branches[$found]} == "${plan_branch[$n]}" ]] || return 1
  checkout_safe "$found" || return 1
  pr_now=$(merged_pr) || return 1
  [[ $pr_now == "${plan_pr[$n]}" ]] || return 1
  collect_state || return 1
  snapshot=$(printf '%s\n' "${state[@]}")
  [[ $snapshot == "${plan_state[$n]}" ]] || return 1
  stop_postgres || return 1
  # Never force removal. Git rechecks local changes and locks before deleting.
  git_at worktree remove "$wt" || return 1
  removed=$((removed + 1))
  [[ ! -e $wt && ! -L $wt ]] || {
    keep 'path recreated; retaining state'
    return 1
  }
  for p in "${state[@]}"; do
    [[ -e $p || -L $p ]] || continue
    safe_state_path "$p" || return 1
    rm -rf -- "$p" || return 1
  done
  echo "REMOVED $wt and ${#state[@]} isolated state path(s)"
}

for ((n = 0; n < ${#plan_path[@]}; n++)); do
  if ! apply_candidate "$n"; then
    echo "CLEANUP INCOMPLETE ${plan_path[$n]}: retained remaining state" >&2
    errors=$((errors + 1))
  fi
done
echo "Removed $removed/${#plan_path[@]} eligible worktree(s)."
[[ $errors == 0 ]]
