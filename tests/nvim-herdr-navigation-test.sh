#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
init_lua="$repo_root/home/config/nvim/init.lua"
neovim_nix="$repo_root/home/user/neovim.nix"

assert_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if ! grep -Fq "$needle" "$file"; then
    printf 'not ok - %s\nmissing: %s\n' "$message" "$needle" >&2
    exit 1
  fi
}

assert_contains 'pname = "herdr-navigator.nvim";' \
  "$neovim_nix" \
  "Home Manager packages the GitHub herdr-navigator.nvim plugin"
assert_contains 'owner = "willfish";' \
  "$neovim_nix" \
  "Home Manager fetches the Neovim navigator plugin from willfish"
assert_contains 'repo = "herdr-navigator.nvim";' \
  "$neovim_nix" \
  "Home Manager fetches the Neovim navigator plugin repository"
assert_contains '"willfish/herdr-navigator.nvim" = customPlugins.herdr-navigator-nvim;' \
  "$neovim_nix" \
  "Home Manager adds the Neovim navigator plugin to the plugin map"
assert_contains '"willfish/herdr-navigator.nvim"' \
  "$neovim_nix" \
  "Home Manager starts the Neovim navigator plugin"
assert_contains 'require("herdr-navigator")' \
  "$init_lua" \
  "Neovim loads the Herdr navigator module"
assert_contains 'herdr_navigator.setup()' \
  "$init_lua" \
  "Neovim initializes Herdr navigator through the plugin"

printf 'ok - nvim-herdr-navigation\n'
