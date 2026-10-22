#!/usr/bin/env bash
# Report pinned upstreams that trail their repositories. Read-only: it never
# edits flake.lock, package files or the store.
set -euo pipefail

usage() {
  cat <<'USAGE'
usage: upstream-status [--check] [FLAKE_DIR]

Compare the revisions in FLAKE_DIR/flake.lock, plus the few artifacts pinned
outside it, with their upstream repositories. FLAKE_DIR defaults to the current
directory.

  --check   Exit 1 when any reported upstream is behind.
  -h        Show this message.

Set GITHUB_TOKEN to raise the GitHub API rate limit. Without it, a run uses
about thirty unauthenticated requests.
USAGE
}

check=false
flake_dir=.
for arg in "$@"; do
  case $arg in
  --check) check=true ;;
  -h | --help)
    usage
    exit 0
    ;;
  -*)
    usage >&2
    exit 2
    ;;
  *) flake_dir=$arg ;;
  esac
done

flake_dir=$(cd "$flake_dir" && pwd)
lock=$flake_dir/flake.lock
if [[ ! -f $lock ]]; then
  echo "upstream-status: no flake.lock in $flake_dir" >&2
  exit 2
fi

api() {
  local path=$1
  if [[ -n ${GITHUB_TOKEN:-} ]]; then
    curl -LfsS -H "Accept: application/vnd.github+json" \
      -H "Authorization: Bearer $GITHUB_TOKEN" "https://api.github.com$path"
  else
    curl -LfsS -H "Accept: application/vnd.github+json" "https://api.github.com$path"
  fi
}

latest_release() {
  local owner=$1 repo=$2
  api "/repos/$owner/$repo/releases/latest" 2>/dev/null | jq -r '.tag_name // empty' 2>/dev/null || true
}

release_commit() {
  local owner=$1 repo=$2 tag=$3
  api "/repos/$owner/$repo/commits/$tag" 2>/dev/null | jq -r '.sha // empty' 2>/dev/null || true
}

head_commit() {
  local owner=$1 repo=$2
  api "/repos/$owner/$repo/commits?per_page=1" 2>/dev/null |
    jq -r '.[0] | "\(.sha) \(.commit.committer.date)"' 2>/dev/null || true
}

fmt_date() {
  local epoch=$1
  if [[ $epoch =~ ^[0-9]+$ ]] && ((epoch > 0)); then
    date -u -d "@$epoch" +%F
  else
    echo "-"
  fi
}

behind=0
printf '%-30s %-10s %-11s %-40s %s\n' INPUT PINNED DATE UPSTREAM RELEASE
while IFS=$'\t' read -r name owner repo rev lastmod; do
  read -r head head_date <<<"$(head_commit "$owner" "$repo")" || true
  release=$(latest_release "$owner" "$repo")
  if [[ $name == herdr-source || $name == hermes-agent ]]; then
    # These two follow upstream release tags through explicit pins in flake.nix,
    # so a newer HEAD is expected; compare against the latest release instead.
    release_sha=$(release_commit "$owner" "$repo" "${release:-HEAD}")
    if [[ -z $release_sha ]]; then
      state='unavailable'
    elif [[ $release_sha == "$rev" ]]; then
      state="release current ${release:--}"
    else
      state="release behind ${release:--}"
      behind=$((behind + 1))
    fi
  elif [[ -z $head ]]; then
    state='unavailable'
  elif [[ $head == "$rev" ]]; then
    state='current'
  else
    state="behind ${head:0:10} ${head_date:0:10}"
    behind=$((behind + 1))
  fi
  printf '%-30s %-10s %-11s %-40s %s\n' \
    "$name" "${rev:0:10}" "$(fmt_date "$lastmod")" "$state" "${release:--}"
done < <(
  jq -r '
    . as $lock
    | $lock.nodes.root.inputs | to_entries[]
    | .key as $name
    | (if (.value | type) == "string" then $lock.nodes[.value].locked else .value end) as $locked
    | select($locked != null and $locked.type == "github")
    | select(($name | test("(^|-)source$")) or $name == "hermes-agent")
    | [$name, $locked.owner, $locked.repo, $locked.rev, ($locked.lastModified // 0)]
    | @tsv
  ' "$lock"
)

echo
printf '%-30s %-10s %-10s %s\n' PACKAGE PINNED LATEST SOURCE
report_package() {
  local label=$1 file=$2 pattern=$3 latest=$4 source=$5
  local pinned
  pinned=$(sed -nE "$pattern" "$flake_dir/$file" | head -n 1)
  local state=current
  if [[ -z $latest ]]; then
    state=unavailable
  elif [[ $pinned != "$latest" ]]; then
    state="behind $latest"
    behind=$((behind + 1))
  fi
  printf '%-30s %-10s %-10s %s\n' "$label" "${pinned:--}" "$state" "$source"
}

agent_browser_latest=$(latest_release vercel-labs agent-browser)
agent_browser_latest=${agent_browser_latest#v}
report_package agent-browser home/user/mcp.nix 's/^ *agentBrowserVersion = "([^"]+)";/\1/p' \
  "$agent_browser_latest" "github:vercel-labs/agent-browser"

report_package rea-agents home/user/mcp-packages/rea-agents.nix \
  's/^ *version = "([^"]+)";/\1/p' \
  "$(curl -LfsS 'https://registry.npmjs.org/rea-agents/latest' 2>/dev/null | jq -r '.version // empty' 2>/dev/null || true)" \
  npm:rea-agents

report_package brave-search-mcp-server home/user/mcp-packages/brave-search-mcp-server.nix \
  's/^ *version = "([^"]+)";/\1/p' \
  "$(curl -LfsS 'https://registry.npmjs.org/@brave%2fbrave-search-mcp-server/latest' 2>/dev/null | jq -r '.version // empty' 2>/dev/null || true)" \
  npm:@brave/brave-search-mcp-server

if [[ $check == true ]] && ((behind > 0)); then
  exit 1
fi
