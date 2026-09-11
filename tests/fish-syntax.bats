#!/usr/bin/env bats

setup() {
  bats_require_minimum_version 1.5.0
  CHECKER="$BATS_TEST_DIRNAME/../scripts/check-fish-syntax"
  FIXTURE=$(mktemp -d)
}

teardown() {
  rm -rf "$FIXTURE"
}

@test "rejects invalid extensionless Fish scripts with supported shebangs" {
  for interpreter in '#! /usr/bin/env fish' '#!/usr/bin/fish' '#!/usr/bin/env -S fish'; do
    printf '%s\nif true\n' "$interpreter" > "$FIXTURE/script"
    run -127 bash "$CHECKER" "$FIXTURE/script"
  done
}

@test "checks fish extensions and ignores other interpreters" {
  printf 'if true\n' > "$FIXTURE/script.fish"
  run -127 bash "$CHECKER" "$FIXTURE/script.fish"

  printf '#!/usr/bin/env bash\nif true; then :; fi\n' > "$FIXTURE/script"
  printf '#! /usr/bin/env fish\nif true; true; end\n' > "$FIXTURE/valid"
  run bash "$CHECKER" "$FIXTURE/script" "$FIXTURE/valid"
  [ "$status" -eq 0 ]
}
