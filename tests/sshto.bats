#!/usr/bin/env bats

setup() {
  export TEST_TMPDIR="$BATS_TEST_TMPDIR"
  export PATH="$TEST_TMPDIR/bin:/usr/bin:/bin"
  mkdir -p "$TEST_TMPDIR/bin"
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
  write_tool fzf '#!/usr/bin/env bash' 'cat >/dev/null; printf "%s\n" mac-mini'

  run env SSHTO_FLAKE="$PWD" SSHTO_EXTRA_HOSTS=mac-mini SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 \
    bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@mac-mini.local" ]
}

@test "uses dscacheutil for host resolution on macOS when getent is unavailable" {
  # shellcheck disable=SC2016
  write_tool realpath '#!/usr/bin/env bash' 'printf "%s\n" "$1"'
  write_tool nix '#!/usr/bin/env bash' 'printf "%s\n" andromeda foundation starfish terminus mac'
  write_tool dscacheutil '#!/usr/bin/env bash' 'case "$*" in *starfish.local*) exit 0 ;; *) exit 1 ;; esac'
  write_tool fzf '#!/usr/bin/env bash' 'cat >/dev/null; printf "%s\n" starfish'

  run env SSHTO_FLAKE="$PWD" SSHTO_DOMAIN=local SSHTO_ASSUME_TTY=1 bash home/config/bin/sshto --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@starfish.local" ]
}

@test "maps host aliases to explicit targets before adding a domain" {
  run env SSHTO_HOST_ALIASES='mac=Williams-Mac-mini.local' \
    bash home/config/bin/sshto mac --domain local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@Williams-Mac-mini.local" ]
}

@test "uses relay as the default Darwin host name" {
  run bash home/config/bin/sshto relay --domain local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@relay.local" ]
}

@test "keeps mac as a compatibility alias for relay" {
  run bash home/config/bin/sshto mac --domain local --print

  [ "$status" -eq 0 ]
  [ "$output" = "herdr --remote william@relay.local" ]
}
