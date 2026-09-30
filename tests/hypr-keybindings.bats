#!/usr/bin/env bats

setup_file() {
  export FLAKE_ROOT="$BATS_TEST_DIRNAME/.."
}

@test "Super+Shift+G toggles agent usage" {
  grep -q 'SUPER SHIFT, G, Toggle agent usage, exec, hypr-controls agents"' \
    "$FLAKE_ROOT/home/config/hyprland/settings.nix"
  if grep -q 'hypr-controls agents show' \
    "$FLAKE_ROOT/home/config/hyprland/settings.nix"; then
    return 1
  fi
  grep -q 'if (panel.opened) panel.close()' \
    "$FLAKE_ROOT/home/config/hyprland/controls/shell.qml"
  grep -q 'call controls toggle' "$FLAKE_ROOT/home/config/hyprland/controls/launch.sh"
}

@test "describes Super+K as the Omarchy keybindings menu" {
  grep -q 'SUPER, K, Keybindings, exec, omarchy-menu-keybindings' \
    "$FLAKE_ROOT/home/config/hyprland/settings.nix"
  grep -q 'bindd =' "$FLAKE_ROOT/home/user/hyprland.nix"
  if grep -q 'settings.bindings.bind$' "$FLAKE_ROOT/home/user/hyprland.nix"; then
    return 1
  fi
}

@test "hosts the pinned Omarchy menu for select mode" {
  grep -q 'plugins/menu' "$FLAKE_ROOT/home/user/hyprland-panels.nix"
  grep -q 'target: "shell"' "$FLAKE_ROOT/home/config/hyprland/controls/shell.qml"
  grep -q 'root.rowsLoaded = true' "$FLAKE_ROOT/home/user/hyprland-panels.nix"
  grep -q 'Copy URL from Web App' "$FLAKE_ROOT/home/user/hyprland-panels.nix"
}

@test "formats described binds like the Omarchy cheatsheet" {
  local source stub
  source="$(nix eval --raw --impure --expr "(builtins.getFlake \"$FLAKE_ROOT\").inputs.omarchy.outPath")"
  stub="$(mktemp -d)"
  cat >"$stub/hyprctl" <<'EOF'
#!/usr/bin/env bash
if [[ $1 == binds ]]; then
  cat <<'BINDS'
bind
	modmask: 64
	submap:
	key: K
	keycode: 0
	catchall: false
	description: Keybindings
	dispatcher: exec
	arg: omarchy-menu-keybindings
bind
	modmask: 64
	submap:
	key: Return
	keycode: 0
	catchall: false
	description: Terminal
	dispatcher: exec
	arg: ghostty
bind
	modmask: 64
	submap:
	key: Q
	keycode: 0
	catchall: false
	description: Close window
	dispatcher: killactive
	arg:
BINDS
  exit 0
fi
if [[ $1 == devices ]]; then
  printf 'active keymap: English (UK)\n'
  exit 0
fi
exit 0
EOF
  printf '#!/usr/bin/env bash\nexit 1\n' >"$stub/xkbcli"
  printf '#!/usr/bin/env bash\nexit 1\n' >"$stub/omarchy-cmd-present"
  chmod +x "$stub/hyprctl" "$stub/xkbcli" "$stub/omarchy-cmd-present"
  run env PATH="$stub:$PATH" XDG_CACHE_HOME="$stub/cache" \
    bash "$source/bin/omarchy-menu-keybindings" --print
  [ "$status" -eq 0 ]
  [[ ${lines[0]} == SUPER\ +\ K*→\ Keybindings ]]
  [[ ${lines[1]} == SUPER\ +\ Return*→\ Terminal ]]
  [[ ${lines[2]} == SUPER\ +\ Q*→\ Close\ window ]]
  rm -rf "$stub"
}
