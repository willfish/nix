#!/usr/bin/env bash
set -euo pipefail

dry_run=0 verbose=0 check_only=0 repo_arg=''
while (($#)); do
  case "$1" in
  --dry-run)
    dry_run=1
    shift
    ;;
  --yes)
    # Retain compatibility with existing callers; cleanup is now the default.
    shift
    ;;
  --verbose)
    verbose=1
    shift
    ;;
  --repo | --check-repo)
    [[ $1 != --check-repo ]] || check_only=1
    [[ $# -ge 2 ]] || exit 2
    repo_arg=$2
    shift 2
    ;;
  -h | --help)
    echo 'usage: wtclean [--dry-run] [--verbose] [--repo PATH]'
    echo 'Remove clean HMRC trade-tariff worktrees whose exact HEAD has a merged or closed PR.'
    echo 'Also stop and remove their isolated flake Postgres and caches. Keep primary checkouts and branches.'
    echo 'Default: clean immediately without confirmation. --dry-run previews without changing anything.'
    echo 'Missing worktree registrations are pruned without deleting unverified state. --verbose explains retained worktrees.'
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
errors=0 removed=0 pruned=0 kept=0 open_count=0 dirty_count=0 unpublished_count=0 other_count=0
reason='' response='' pr='' pr_kind='' commit_missing=0
real_git="$(command git --exec-path)/../../bin/git"
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
declare -a warnings=()
declare -a plan_repo=() plan_path=() plan_rev=() plan_branch=() plan_remote=() plan_default=() plan_pr=() plan_state=()
declare -a paths=() revs=() branches=() blocked=() state=()

git_at() { "$real_git" -c core.fsmonitor=false -C "$repo" "$@"; }
keep() { reason="$*"; }
warn() {
  warnings+=("$*")
  errors=$((errors + 1))
}
report_keep() {
  kept=$((kept + 1))
  case "$reason" in
  'open PR') open_count=$((open_count + 1)) ;;
  'local changes or untracked files' | 'assume-unchanged or skip-worktree index entries') dirty_count=$((dirty_count + 1)) ;;
  'unpublished review snapshot' | 'no associated PR') unpublished_count=$((unpublished_count + 1)) ;;
  *) other_count=$((other_count + 1)) ;;
  esac
  if [[ $verbose == 1 ]]; then printf 'KEEP %s: %s\n' "$wt" "$reason"; fi
}
fetch_json() {
  commit_missing=0
  if ! gh api "$@" >"$scratch/response" 2>"$scratch/error"; then
    reason=$(sed -n 's/^gh: //p' "$scratch/error" | head -n 1)
    [[ -n $reason ]] || reason='GitHub request failed'
    if [[ $reason == 'No commit found for SHA:'* && $reason == *'(HTTP 422)' ]]; then commit_missing=1; fi
    return 1
  fi
  response=$(cat "$scratch/response")
}
summary() {
  if [[ $dry_run == 1 ]]; then
    printf 'Would remove %s worktrees; would prune %s stale registrations.\n' "${#plan_path[@]}" "$pruned"
  else
    printf 'Removed %s worktrees; pruned %s stale registrations.\n' "$removed" "$pruned"
  fi
  printf 'Kept %s worktrees: %s linked to open PRs, %s modified, %s unpublished/no PR, %s protected/unverified.\n' "$kept" "$open_count" "$dirty_count" "$unpublished_count" "$other_count"
  if [[ $errors != 0 ]]; then
    printf 'Incomplete: %s checks or cleanup actions failed:\n' "$errors" >&2
    printf '  %s\n' "${warnings[@]}" >&2
  fi
}

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
    locked | locked\ *) block=locked ;;
    prunable | prunable\ *) [[ $block == locked ]] || block=missing ;;
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

closed_pr() {
  local branch=$1 endpoint encoded match
  local -a endpoints=()
  if [[ -n $branch ]]; then
    encoded=$(jq -rn --arg head "trade-tariff:${branch#refs/heads/}" '$head | @uri') || return 1
    endpoints+=("repos/$remote/pulls?state=all&head=$encoded&per_page=100")
  fi
  # Review checkouts can have another name, or lose remote reachability after
  # squash merging. Ask GitHub before classifying them as local-only snapshots.
  endpoints+=("repos/$remote/commits/$rev/pulls?per_page=100")
  for endpoint in "${endpoints[@]}"; do
    if ! fetch_json "$endpoint"; then
      if [[ $commit_missing == 1 && $endpoint == */commits/* ]]; then
        keep 'unpublished review snapshot'
        return 1
      fi
      warn "$remote ($wt): $reason"
      keep 'GitHub lookup failed'
      return 1
    fi
    if ! jq -e 'type == "array" and length < 100' <<<"$response" >/dev/null 2>&1; then
      warn "$remote ($wt): ambiguous or invalid PR response"
      keep 'GitHub lookup failed'
      return 1
    fi
    if jq -e 'any(.[]; .state == "open")' <<<"$response" >/dev/null; then
      keep 'open PR'
      return 1
    fi
    if [[ $(jq length <<<"$response") != 0 ]]; then break; fi
  done
  if [[ $(jq length <<<"$response") == 0 ]]; then
    keep 'no associated PR'
    return 1
  fi
  match=$(jq -er --arg rev "$rev" --arg remote "$remote" '
    map(select(.state == "closed" and .head.sha == $rev
      and .head.repo.full_name == $remote and .base.repo.full_name == $remote))
    | select(length == 1) | .[0]
    | select(.number | type == "number" and . > 0 and floor == .)
    | [.number, (if .merged_at then "merged" else "closed" end)] | @tsv
  ' <<<"$response") || {
    keep 'PR tip differs from checkout, or is ambiguous'
    return 1
  }
  IFS=$'\t' read -r pr pr_kind <<<"$match"
}

unverified_repo() {
  local i
  warn "$remote: $reason"
  for ((i = 1; i < ${#paths[@]}; i++)); do
    [[ ${blocked[$i]} != missing ]] || continue
    wt=${paths[$i]}
    keep 'repository lookup failed'
    report_keep
  done
}

prune_missing() {
  local preview count
  preview=$(git_at worktree prune --dry-run --verbose --expire now 2>&1) || {
    warn "$remote: cannot inspect stale worktree registrations"
    return 1
  }
  count=$(grep -c '^Removing ' <<<"$preview" || true)
  [[ $count != 0 ]] || return 0
  if [[ $dry_run != 1 ]]; then
    # Use real Git, not the service-state wrapper: missing records prove no PR state.
    git_at worktree prune --expire now 2>"$scratch/error" || {
      warn "$remote: cannot prune stale worktree registrations"
      return 1
    }
  fi
  pruned=$((pruned + count))
  if [[ $verbose == 1 ]]; then printf '%s\n' "$preview"; fi
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
    warn "$remote: cannot list registered worktrees"
    continue
  }
  [[ ${#paths[@]} -gt 1 ]] || continue
  prune_missing || continue
  if [[ $dry_run != 1 ]]; then
    load_worktrees || {
      warn "$remote: cannot reload worktrees after pruning"
      continue
    }
  fi
  [[ ${#paths[@]} -gt 1 ]] || continue
  if ! fetch_json "repos/$remote" --jq .default_branch; then
    unverified_repo
    continue
  fi
  default=$response
  [[ -n $default && $default != null ]] || {
    warn "$remote: default branch is unknown"
    continue
  }
  for ((i = 1; i < ${#paths[@]}; i++)); do
    # Dry runs keep the registrations, but report them as pending pruning.
    [[ ${blocked[$i]} != missing ]] || continue
    reason='cannot inspect worktree'
    checkout_safe "$i" || {
      report_keep
      continue
    }
    rev=${revs[$i]}
    if ! closed_pr "${branches[$i]}"; then
      report_keep
      continue
    fi
    collect_state || {
      report_keep
      continue
    }
    if [[ $dry_run == 1 || $verbose == 1 ]]; then printf 'REMOVE %s (%s PR #%s)\n' "$wt" "$pr_kind" "$pr"; fi
    if [[ $verbose == 1 ]]; then for p in "${state[@]}"; do echo "  state: $p"; done; fi
    plan_repo+=("$repo") plan_path+=("$wt") plan_rev+=("$rev") plan_branch+=("${branches[$i]}")
    plan_remote+=("$remote") plan_default+=("$default") plan_pr+=("$pr")
    snapshot=$(printf '%s\n' "${state[@]}")
    plan_state+=("$snapshot")
  done
done

if [[ $dry_run == 1 || ${#plan_path[@]} == 0 ]]; then
  summary
  [[ $errors == 0 ]]
  exit
fi
apply_candidate() {
  local n=$1 i found=-1 snapshot p
  repo=${plan_repo[$n]} wt=${plan_path[$n]} rev=${plan_rev[$n]}
  remote=${plan_remote[$n]} default=${plan_default[$n]}
  reason=''
  load_worktrees || return 1
  for ((i = 1; i < ${#paths[@]}; i++)); do
    [[ ${paths[$i]} != "$wt" ]] || {
      found=$i
      break
    }
  done
  [[ $found != -1 && ${revs[$found]} == "$rev" && ${branches[$found]} == "${plan_branch[$n]}" ]] || return 1
  checkout_safe "$found" || return 1
  closed_pr "${branches[$found]}" || return 1
  [[ $pr == "${plan_pr[$n]}" ]] || return 1
  collect_state || return 1
  snapshot=$(printf '%s\n' "${state[@]}")
  [[ $snapshot == "${plan_state[$n]}" ]] || return 1
  stop_postgres >"$scratch/postgres" 2>&1 || return 1
  if [[ $verbose == 1 ]]; then cat "$scratch/postgres"; fi
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
  printf 'REMOVED %s (%s PR #%s) and %s isolated state path(s)\n' "$wt" "$pr_kind" "$pr" "${#state[@]}"
}

for ((n = 0; n < ${#plan_path[@]}; n++)); do
  if ! apply_candidate "$n"; then
    warn "${plan_path[$n]}: cleanup incomplete; retained remaining state${reason:+ ($reason)}"
  fi
done
summary
[[ $errors == 0 ]]
