#!/usr/bin/env bats

setup() {
  bats_require_minimum_version 1.5.0
  SCRIPT="$BATS_TEST_DIRNAME/../home/config/hyprland/record.sh"
  FIXTURE="$BATS_TEST_TMPDIR/hypr-record"
  mkdir -p "$FIXTURE/bin" "$FIXTURE/runtime" "$FIXTURE/home"
  cat >"$FIXTURE/bin/notify-send" <<'EOF'
#!/bin/sh
printf '%s\n' "$*"
EOF
  cat >"$FIXTURE/bin/slurp" <<'EOF'
#!/bin/sh
printf '%s\n' '1 1 0 0'
EOF
  cat >"$FIXTURE/bin/pkill" <<'EOF'
#!/bin/sh
exit 0
EOF
  chmod +x "$FIXTURE/bin/"*
  export PATH="$FIXTURE/bin:$PATH"
  export XDG_RUNTIME_DIR="$FIXTURE/runtime"
  export HOME="$FIXTURE/home"
  export XDG_CONFIG_HOME="$FIXTURE/home"
  export HYPR_RECORD_KMS_SERVER="$FIXTURE/missing-kms"
}

run_record() {
  run bash -o errexit -o nounset -o pipefail "$SCRIPT"
}

@test "a failed start does not leave the next press busy" {
  touch "$HYPR_RECORD_KMS_SERVER"
  chmod +x "$HYPR_RECORD_KMS_SERVER"

  run_record
  [ "$status" -eq 1 ]
  [[ "$output" == *"too small"* ]]
  [ ! -d "$XDG_RUNTIME_DIR/hypr-record/lock" ]

  run_record
  [ "$status" -eq 1 ]
  [[ "$output" == *"too small"* ]]
  [[ "$output" != *"Already busy"* ]]
  [ ! -d "$XDG_RUNTIME_DIR/hypr-record/lock" ]
}

@test "the camera square sits in the bottom-right of the region" {
  run bash -o errexit -o nounset -o pipefail "$SCRIPT" box 1920 1080 100 40
  [ "$status" -eq 0 ]
  [ "$output" = "1726 826 270" ]
}

@test "a leftover lock does not block a new recording" {
  mkdir -p "$XDG_RUNTIME_DIR/hypr-record/lock"
  touch "$HYPR_RECORD_KMS_SERVER"
  chmod +x "$HYPR_RECORD_KMS_SERVER"

  run_record
  [ "$status" -eq 1 ]
  [[ "$output" == *"too small"* ]]
  [[ "$output" != *"Already busy"* ]]
  [ ! -d "$XDG_RUNTIME_DIR/hypr-record/lock" ]
}
