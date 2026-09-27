#!/usr/bin/env bats

setup() {
  root="$(cd "$BATS_TEST_DIRNAME/.." && pwd)"
  export HOME="$BATS_TEST_TMPDIR/home"
  mkdir -p "$HOME" "$BATS_TEST_TMPDIR/probe-temp"
  export TMPDIR="$BATS_TEST_TMPDIR/probe-temp"
  ssh-keygen -q -t ed25519 -N '' -f "$BATS_TEST_TMPDIR/authorized"
  ssh-keygen -q -t ed25519 -N '' -f "$BATS_TEST_TMPDIR/unrelated"
  recipient=$(ssh-to-age -i "$BATS_TEST_TMPDIR/authorized.pub")
  printf 'fixture: synthetic-test-only\n' > "$BATS_TEST_TMPDIR/plain.yaml"
  sops --encrypt --age "$recipient" --input-type yaml --output-type yaml \
    "$BATS_TEST_TMPDIR/plain.yaml" > "$BATS_TEST_TMPDIR/encrypted.yaml"
}

@test "a missing SSH identity fails closed" {
  run bash "$root/scripts/probe-private-access" "$BATS_TEST_TMPDIR/missing" "$BATS_TEST_TMPDIR/encrypted.yaml"
  [ "$status" -ne 0 ]
  [ -z "$output" ]
}

@test "an unrelated SSH identity fails even with ambient authorized age credentials" {
  export SOPS_AGE_KEY
  SOPS_AGE_KEY=$(ssh-to-age -private-key -i "$BATS_TEST_TMPDIR/authorized")
  run bash "$root/scripts/probe-private-access" "$BATS_TEST_TMPDIR/unrelated" "$BATS_TEST_TMPDIR/encrypted.yaml"
  [ "$status" -ne 0 ]
  [ -z "$output" ]
  [ -z "$(ls -A "$TMPDIR")" ]
}

@test "the authorized SSH identity succeeds without plaintext output or retained keys" {
  run bash "$root/scripts/probe-private-access" "$BATS_TEST_TMPDIR/authorized" "$BATS_TEST_TMPDIR/encrypted.yaml"
  [ "$status" -eq 0 ]
  [ -z "$output" ]
  [ -z "$(ls -A "$TMPDIR")" ]
}

@test "a passphrase-protected SSH key fails without prompting" {
  ssh-keygen -q -t ed25519 -N 'synthetic-passphrase' -f "$BATS_TEST_TMPDIR/locked"
  run timeout 5 bash "$root/scripts/probe-private-access" "$BATS_TEST_TMPDIR/locked" "$BATS_TEST_TMPDIR/encrypted.yaml"
  [ "$status" -ne 0 ]
  [ "$status" -ne 124 ]
  [ -z "$output" ]
  [ -z "$(ls -A "$TMPDIR")" ]
}

@test "corrupt key and corrupt encrypted file fail closed" {
  printf 'invalid\n' > "$BATS_TEST_TMPDIR/corrupt"
  run bash "$root/scripts/probe-private-access" "$BATS_TEST_TMPDIR/corrupt" "$BATS_TEST_TMPDIR/encrypted.yaml"
  [ "$status" -ne 0 ]
  [ -z "$output" ]
  run bash "$root/scripts/probe-private-access" "$BATS_TEST_TMPDIR/authorized" "$BATS_TEST_TMPDIR/corrupt"
  [ "$status" -ne 0 ]
  [ -z "$output" ]
  [ -z "$(ls -A "$TMPDIR")" ]
}
