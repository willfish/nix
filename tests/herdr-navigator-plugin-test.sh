#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

assert_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if ! grep -Fq "$needle" "$file"; then
    printf 'not ok - %s\nmissing: %s\nfile: %s\n' "$message" "$needle" "$file" >&2
    exit 1
  fi
}

assert_not_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if grep -Fq "$needle" "$file"; then
    printf 'not ok - %s\nunexpected: %s\nfile: %s\n' "$message" "$needle" "$file" >&2
    exit 1
  fi
}

assert_not_exists() {
  local path="$1"
  local message="$2"
  if [ -e "$path" ]; then
    printf 'not ok - %s\nunexpected path: %s\n' "$message" "$path" >&2
    exit 1
  fi
}

assert_contains 'command = "willfish.herdr-navigator.left"' \
  "$repo_root/home/config/herdr/config.toml" \
  "alt+h invokes the Herdr navigator plugin action"
assert_contains 'command = "willfish.herdr-navigator.down"' \
  "$repo_root/home/config/herdr/config.toml" \
  "alt+j invokes the Herdr navigator plugin action"
assert_contains 'command = "willfish.herdr-navigator.up"' \
  "$repo_root/home/config/herdr/config.toml" \
  "alt+k invokes the Herdr navigator plugin action"
assert_contains 'command = "willfish.herdr-navigator.right"' \
  "$repo_root/home/config/herdr/config.toml" \
  "alt+l invokes the Herdr navigator plugin action"
assert_contains 'type = "plugin_action"' \
  "$repo_root/home/config/herdr/config.toml" \
  "navigator keybindings use Herdr plugin actions"
assert_not_contains 'herdr-navigate ' \
  "$repo_root/home/config/herdr/config.toml" \
  "Herdr config no longer calls the local herdr-navigate script"

assert_contains 'plugin install willfish/herdr-navigator --yes' \
  "$repo_root/home/user/config.nix" \
  "Home Manager installs the GitHub herdr-navigator plugin"
assert_contains 'plugin list --plugin willfish.herdr-navigator --json' \
  "$repo_root/home/user/config.nix" \
  "Home Manager detects whether herdr-navigator is GitHub-backed"
# shellcheck disable=SC2016
assert_contains 'herdrBin="${pkgs.herdr}/bin/herdr"' \
  "$repo_root/home/user/config.nix" \
  "Home Manager uses the Nix-managed Herdr binary during activation"

assert_not_exists "$repo_root/home/config/bin/herdr-navigate" \
  "Herdr pane navigation implementation lives in the plugin repo"

printf 'ok - herdr-navigator-plugin\n'
