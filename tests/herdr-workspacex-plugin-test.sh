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

assert_not_exists() {
  local path="$1"
  local message="$2"
  if [ -e "$path" ]; then
    printf 'not ok - %s\nunexpected path: %s\n' "$message" "$path" >&2
    exit 1
  fi
}

assert_contains 'command = "herdr plugin pane open --plugin willfish.herdr-workspacex --entrypoint picker --placement overlay --focus"' \
  "$repo_root/home/config/herdr/config.toml" \
  "prefix+o opens the workspacex plugin picker pane"

assert_contains 'type = "shell"' \
  "$repo_root/home/config/herdr/config.toml" \
  "prefix+o runs the Herdr plugin control command as a shell command"

assert_contains 'plugin install willfish/herdr-workspacex --yes' \
  "$repo_root/home/user/config.nix" \
  "Home Manager installs the GitHub herdr-workspacex plugin"

assert_contains 'nix shell nixpkgs#cargo nixpkgs#rustc' \
  "$repo_root/home/user/config.nix" \
  "Home Manager can build the GitHub plugin without persistent cargo on PATH"

assert_contains '"kind":"github"' \
  "$repo_root/home/user/config.nix" \
  "Home Manager only reinstalls when the plugin is not GitHub-backed"

assert_contains 'plugin unlink fish.herdr-workspacex' \
  "$repo_root/home/user/config.nix" \
  "Home Manager unlinks the old fish plugin id"

assert_contains 'server reload-config' \
  "$repo_root/home/user/config.nix" \
  "Home Manager reloads the running Herdr server config"

assert_not_exists "$repo_root/home/config/bin/herdr-zoxide-workspace" \
  "workspace picker implementation lives in the plugin repo"

printf 'ok - herdr-workspacex-plugin\n'
