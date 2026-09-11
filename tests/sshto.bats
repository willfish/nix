#!/usr/bin/env bats
# shellcheck disable=SC2016

setup() {
  bats_require_minimum_version 1.5.0
  export TEST_TMPDIR="$BATS_TEST_TMPDIR"
  export PATH="$TEST_TMPDIR/bin:$PATH"
  mkdir -p "$TEST_TMPDIR/bin"
  write_tool hostname '#!/usr/bin/env bash' 'printf "%s\n" fixture-host'
  write_tool getent '#!/usr/bin/env bash' 'exit 1'
  write_tool nc '#!/usr/bin/env bash' 'exit 1'
  write_tool tailscale '#!/usr/bin/env bash' 'exit 1'
}

write_tool() {
  local name="$1"
  shift
  printf '%s\n' "$@" > "$TEST_TMPDIR/bin/$name"
  chmod +x "$TEST_TMPDIR/bin/$name"
}

@test "discovers extra hosts when no flake host matches a future Mac" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation starfish terminus'
  write_tool dscacheutil '#!/usr/bin/env bash' 'case "$*" in *mac-mini*) exit 0 ;; *) exit 1 ;; esac'
  write_tool nc '#!/usr/bin/env bash' 'exit 0'
  write_tool fzf '#!/usr/bin/env bash' 'awk -F "\t" '\''$2 == "mac-mini" { print; exit }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_EXTRA_HOSTS=mac-mini SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@mac-mini.local --remote-keybindings server" ]
}

@test "uses dscacheutil for host resolution on macOS when getent is unavailable" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation starfish terminus mac'
  write_tool dscacheutil '#!/usr/bin/env bash' 'case "$*" in *starfish.local*) exit 0 ;; *) exit 1 ;; esac'
  write_tool nc '#!/usr/bin/env bash' 'exit 0'
  write_tool fzf '#!/usr/bin/env bash' 'awk -F "\t" '\''$2 == "starfish" { print; exit }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@starfish.local --remote-keybindings server" ]
}

@test "does not concatenate aliases onto a flake host without trailing newline" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s" "andromeda
foundation
starfish
terminus
relay
andromeda
foundation
starfish
terminus"'
  write_tool dscacheutil '#!/usr/bin/env bash' 'exit 0'
  write_tool nc '#!/usr/bin/env bash' 'exit 0'
  write_tool fzf '#!/usr/bin/env bash' 'awk -F "\t" '\''$2 == "terminus" { row = $0 } /terminusmac/ { bad = 1 } END { if (bad) exit 42; if (row != "") print row }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@terminus.local --remote-keybindings server" ]
}

@test "maps host aliases to explicit targets before adding a domain" {
  run env SSHTO_HOST_ALIASES='mac=Williams-Mac-mini.local' \
    bash home/config/bin/sshto mac --domain local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@Williams-Mac-mini.local --remote-keybindings server" ]
}

@test "uses relay as the default Darwin host name" {
  run bash home/config/bin/sshto relay --domain local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@relay.local --remote-keybindings server" ]
}

@test "treats mac as a regular host name" {
  run bash home/config/bin/sshto mac --domain local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@mac.local --remote-keybindings server" ]
}

@test "uses server keybindings by default" {
  run bash home/config/bin/sshto andromeda --domain local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@andromeda.local --remote-keybindings server" ]
}

@test "allows local keybindings to override the server default" {
  run bash home/config/bin/sshto andromeda --domain local --keybindings local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@andromeda.local --remote-keybindings local" ]
}

@test "prefers and labels a resolvable LAN route over Tailscale" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation'
  write_tool getent '#!/usr/bin/env bash' 'case "$*" in *andromeda.local*) exit 0 ;; *) exit 1 ;; esac'
  write_tool nc '#!/usr/bin/env bash' 'case "$*" in *andromeda.local*22*) exit 0 ;; *) exit 1 ;; esac'
  write_tool tailscale '#!/usr/bin/env bash' 'printf "%s\n" '\''{"Peer":{"peer":{"HostName":"andromeda","DNSName":"andromeda.example.ts.net.","Online":true}}}'\'''
  write_tool fzf '#!/usr/bin/env bash' 'tee "$TEST_TMPDIR/fzf-input" | awk -F "\t" '\''$2 == "andromeda" { print; exit }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@andromeda.local --remote-keybindings server" ]
  grep -F $'andromeda  [LAN]\tandromeda\tandromeda.local' "$TEST_TMPDIR/fzf-input"
}

@test "falls back to Tailscale when LAN DNS resolves but port 22 is unreachable" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation'
  write_tool getent '#!/usr/bin/env bash' 'case "$*" in *andromeda.local*) exit 0 ;; *) exit 1 ;; esac'
  write_tool nc '#!/usr/bin/env bash' 'exit 1'
  write_tool tailscale '#!/usr/bin/env bash' 'printf "%s\n" '\''{"Peer":{"peer":{"HostName":"andromeda","DNSName":"andromeda.example.ts.net.","Online":true}}}'\'''
  write_tool fzf '#!/usr/bin/env bash' 'awk -F "\t" '\''$2 == "andromeda" { print; exit }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@andromeda.example.ts.net --remote-keybindings server" ]
}

@test "falls back to and labels an online Tailscale peer" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation'
  write_tool getent '#!/usr/bin/env bash' 'exit 1'
  write_tool tailscale '#!/usr/bin/env bash' 'printf "%s\n" '\''{"Peer":{"peer":{"HostName":"andromeda","DNSName":"andromeda.example.ts.net.","Online":true}}}'\'''
  write_tool fzf '#!/usr/bin/env bash' 'tee "$TEST_TMPDIR/fzf-input" | awk -F "\t" '\''$2 == "andromeda" { print; exit }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@andromeda.example.ts.net --remote-keybindings server" ]
  grep -F $'andromeda  [Tailscale]\tandromeda\tandromeda.example.ts.net' "$TEST_TMPDIR/fzf-input"
}

@test "excludes offline Tailscale peers from the picker" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation terminus'
  write_tool getent '#!/usr/bin/env bash' 'case "$*" in *andromeda.local*) exit 0 ;; *) exit 1 ;; esac'
  write_tool nc '#!/usr/bin/env bash' 'exit 1'
  write_tool tailscale '#!/usr/bin/env bash' 'printf "%s\n" '\''{"Peer":{"offline":{"HostName":"andromeda","DNSName":"andromeda.example.ts.net.","Online":false},"online":{"HostName":"terminus","DNSName":"terminus.example.ts.net.","Online":true}}}'\'''
  write_tool fzf '#!/usr/bin/env bash' 'tee "$TEST_TMPDIR/fzf-input" | awk -F "\t" '\''$2 == "terminus" { print; exit }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@terminus.example.ts.net --remote-keybindings server" ]
  run grep -F 'andromeda' "$TEST_TMPDIR/fzf-input"
  [ "$status" -ne 0 ]
  grep -F $'terminus  [Tailscale]\tterminus\tterminus.example.ts.net' "$TEST_TMPDIR/fzf-input"
}

@test "keeps LAN discovery working when Tailscale status fails" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation'
  write_tool getent '#!/usr/bin/env bash' 'case "$*" in *andromeda.local*) exit 0 ;; *) exit 1 ;; esac'
  write_tool nc '#!/usr/bin/env bash' 'exit 0'
  write_tool tailscale '#!/usr/bin/env bash' 'exit 1'
  write_tool fzf '#!/usr/bin/env bash' 'awk -F "\t" '\''$2 == "andromeda" { print; exit }'\'''

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@andromeda.local --remote-keybindings server" ]
}

@test "excludes the current host from the remote picker" {
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda terminus'
  write_tool hostname '#!/usr/bin/env bash' 'printf "%s\n" andromeda'
  write_tool getent '#!/usr/bin/env bash' 'exit 0'
  write_tool nc '#!/usr/bin/env bash' 'exit 0'
  write_tool fzf '#!/usr/bin/env bash' 'tee "$TEST_TMPDIR/fzf-input" | head -n1'

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@terminus.local --remote-keybindings server" ]
  run ! grep -F andromeda "$TEST_TMPDIR/fzf-input"
}

@test "ignores a Tailscale search suffix when detecting the LAN domain" {
  write_tool resolvectl '#!/usr/bin/env bash' 'exit 1'
  write_tool hostname '#!/usr/bin/env bash' 'printf "%s\n" "(none)"'
  write_tool getent '#!/usr/bin/env bash' 'case "$*" in *andromeda.lan.example*) exit 0 ;; *) exit 1 ;; esac'
  write_tool nc '#!/usr/bin/env bash' 'exit 0'
  write_tool tailscale '#!/usr/bin/env bash' 'exit 1'
  printf '%s\n' 'search taile09696.ts.net lan.example' > "$TEST_TMPDIR/resolv.conf"

  run env SSHTO_RESOLV_CONF="$TEST_TMPDIR/resolv.conf" \
    bash home/config/bin/sshto andromeda --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@andromeda.lan.example --remote-keybindings server" ]
}
