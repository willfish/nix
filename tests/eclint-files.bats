#!/usr/bin/env bats

setup() {
  export ECLINT_FILES="${ECLINT_FILES:-$BATS_TEST_DIRNAME/../scripts/eclint-files}"
  cd "$BATS_TEST_TMPDIR" || return 1
  printf 'root = true\n[*]\ninsert_final_newline = true\n' > .editorconfig
  printf 'clean\n' > 'clean file.py'
  printf 'also clean\n' > second.py
  printf 'missing newline' > 'bad file.py'
  printf 'also missing newline' > '--fix'
}

@test "checks a failing second file after a clean first file" {
  run "$ECLINT_FILES" 'clean file.py' 'bad file.py'
  [ "$status" -eq 1 ]
  [[ "$output" == *'bad file.py'* ]]
}

@test "reports failures regardless of file order" {
  run "$ECLINT_FILES" 'bad file.py' 'clean file.py'
  [ "$status" -eq 1 ]
  [[ "$output" == *'bad file.py'* ]]
}

@test "checks every failing file without treating a filename as an option" {
  run "$ECLINT_FILES" 'bad file.py' '--fix' 'clean file.py'
  [ "$status" -eq 1 ]
  [[ "$output" == *'bad file.py'* ]]
  [[ "$output" == *'--fix'* ]]
  [ "$(wc -c < '--fix')" -eq 20 ]
}

@test "accepts multiple clean files" {
  run "$ECLINT_FILES" 'clean file.py' second.py
  [ "$status" -eq 0 ]
}
