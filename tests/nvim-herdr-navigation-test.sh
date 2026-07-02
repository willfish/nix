#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
init_lua="$repo_root/home/config/nvim/init.lua"

assert_contains() {
  local needle="$1"
  local message="$2"
  if ! grep -Fq "$needle" "$init_lua"; then
    printf 'not ok - %s\nmissing: %s\n' "$message" "$needle" >&2
    exit 1
  fi
}

assert_contains 'local in_herdr = vim.env.HERDR_SESSION or vim.env.HERDR_PANE_ID or vim.env.HERDR_ENV' \
  "Neovim detects Herdr panes without HERDR_SESSION"
assert_contains 'local pane_id = vim.env.HERDR_PANE_ID' \
  "Neovim uses the explicit Herdr pane id when focusing adjacent panes"
assert_contains 'vim.fn.system({ "herdr", "pane", "focus", "--direction", herdr_direction, "--pane", pane_id })' \
  "Neovim asks Herdr to focus from its current pane"
assert_contains 'if in_herdr then' \
  "Herdr mappings are installed when any Herdr pane marker exists"

printf 'ok - nvim-herdr-navigation\n'
