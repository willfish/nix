#!/usr/bin/env bats
# shellcheck disable=SC2016

setup() {
  export WIFI_FIXTURE="$BATS_TEST_TMPDIR"
  export PATH="$WIFI_FIXTURE/bin:$PATH"
  mkdir -p "$WIFI_FIXTURE/bin"
  cat > "$WIFI_FIXTURE/bin/nmcli" <<'SH'
#!/usr/bin/env bash
case "$*" in
  '-g connection.interface-name connection show FishFamille') printf 'wlan0\n' ;;
  '-g 802-11-wireless.bssid connection show FishFamille') printf '00:00:00:00:00:01\n' ;;
  connection\ modify*) printf '%s\n' "$5" >> "$WIFI_FIXTURE/pins" ;;
esac
SH
  cat > "$WIFI_FIXTURE/bin/sleep" <<'SH'
#!/usr/bin/env bash
if [ -n "${WIFI_SIGNAL:-}" ] && [ ! -e "$WIFI_FIXTURE/signalled" ]; then
  touch "$WIFI_FIXTURE/signalled"
  kill -s "$WIFI_SIGNAL" "$PPID"
fi
exit "${WIFI_SLEEP_EXIT:-0}"
SH
  for tool in journalctl ip ping; do
    printf '#!/usr/bin/env bash\nexit 0\n' > "$WIFI_FIXTURE/bin/$tool"
  done
  chmod +x "$WIFI_FIXTURE/bin/"*
}

run_roam() {
  local signal="$1" sleep_exit="$2"
  shift 2
  run env WIFI_SIGNAL="$signal" WIFI_SLEEP_EXIT="$sleep_exit" bash "$BATS_TEST_DIRNAME/../home/config/bin/wifi-roam-test" \
    --duration 0 --settle 0 "$@" 00:00:00:00:00:02 00:00:00:00:00:03
}

assert_signal_restore() {
  [ "$(cat "$WIFI_FIXTURE/pins")" = $'00:00:00:00:00:02\n00:00:00:00:00:01' ]
}

@test "SIGTERM restores once and stops before the second pin" {
  run_roam TERM 0
  [ "$status" -eq 143 ]
  assert_signal_restore
}

@test "SIGINT restores once and stops before the second pin" {
  run_roam INT 0
  [ "$status" -eq 130 ]
  assert_signal_restore
}

@test "normal completion tests every pin then restores once" {
  run_roam "" 0
  [ "$status" -eq 0 ]
  [ "$(cat "$WIFI_FIXTURE/pins")" = $'00:00:00:00:00:02\n00:00:00:00:00:03\n00:00:00:00:00:01' ]
}

@test "keep-last still retains the final pin on normal completion" {
  run_roam "" 0 --keep-last
  [ "$status" -eq 0 ]
  [ "$(cat "$WIFI_FIXTURE/pins")" = $'00:00:00:00:00:02\n00:00:00:00:00:03' ]
}

@test "an interrupted keep-last run restores the original pin" {
  run_roam TERM 0 --keep-last
  [ "$status" -eq 143 ]
  assert_signal_restore
}

@test "failure keeps its exit status and restores the original pin" {
  run_roam "" 23
  [ "$status" -eq 23 ]
  assert_signal_restore
}
