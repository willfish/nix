#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  echo "usage: mcp-update [FLAKE_DIR]"
  echo "Resolve current MCP releases to immutable flake inputs and refresh the npm runtime lock."
  exit 0
fi
if (($# > 1)); then
  echo "usage: mcp-update [FLAKE_DIR]" >&2
  exit 2
fi

root=$(cd "${1:-.}" && pwd)
lock="$root/flake.lock"
packages="$root/home/user/mcp-packages"
work=$(mktemp -d "${TMPDIR:-/tmp}/mcp-update.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
cp "$lock" "$work/flake.lock"

curl --fail --silent --show-error --location \
  https://registry.npmjs.org/@brave/brave-search-mcp-server/latest >"$work/search.json"

lock_file() {
  local input=$1 url=$2 result
  result=$(nix flake prefetch --json "file+$url")
  jq --arg input "$input" --argjson fetched "$result" '
    .nodes.root.inputs[$input] as $node
    | if ($node | type) != "string" then error("missing MCP input: " + $input)
      else .nodes[$node].locked = $fetched.locked end
  ' "$work/flake.lock" >"$work/next.lock"
  mv "$work/next.lock" "$work/flake.lock"
}

# Agent Browser is a patched source build. Update its explicit source tag,
# rebase the patch, and refresh the dashboard dependency hash before switching.

version=$(jq -er '.version | select(test("^[0-9]+\\.[0-9]+\\.[0-9]+([+-][A-Za-z0-9.-]+)?$"))' "$work/search.json")
lock_file brave-search-release "https://registry.npmjs.org/@brave/brave-search-mcp-server/$version"

# Match the production-only manifest used by the Nix package. Never execute
# upstream prepare/install scripts, and do not churn transitive versions when
# the released manifest has not changed.
jq '{name: "brave-search-mcp-server", version, bin, dependencies}' \
  "$work/search.json" >"$work/package.json"
jq -S . "$packages/brave-search-mcp-package.json" >"$work/old-package.json"
jq -S . "$work/package.json" >"$work/new-package.json"
refresh=false
if ! cmp -s "$work/old-package.json" "$work/new-package.json"; then
  refresh=true
  cp "$packages/brave-search-mcp-package-lock.json" "$work/package-lock.json"
  (
    cd "$work"
    npm install --package-lock-only --ignore-scripts --no-audit --no-fund \
      --registry=https://registry.npmjs.org --cache "$work/npm-cache"
  )
  hash=$(prefetch-npm-deps "$work/package-lock.json")
  [[ $hash =~ ^sha256-[A-Za-z0-9+/]+=*$ ]]
  jq -n --arg hash "$hash" '{hash: $hash}' >"$work/npm-deps.json"
fi

# Publish only after every download and dependency resolution succeeded.
if [[ $refresh == true ]]; then
  install -m 644 "$work/package.json" "$packages/brave-search-mcp-package.json"
  install -m 644 "$work/package-lock.json" "$packages/brave-search-mcp-package-lock.json"
  install -m 644 "$work/npm-deps.json" "$packages/brave-search-mcp-deps.json"
fi
install -m 644 "$work/flake.lock" "$lock"
printf 'Kept the reviewed Agent Browser source pin; locked Brave Search %s\n' "$version"
