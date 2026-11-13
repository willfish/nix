#!/usr/bin/env bash
set -euo pipefail
script=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/audio-controls.sh
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
export AUDIO_TEST_LOG="$work/commands"
export AUDIO_TEST_NODES="$work/nodes.json"
cat >"$AUDIO_TEST_NODES" <<'EOF'
[
  {"id":1,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Sink"},"params":{"Props":[{"mute":false}]}}},
  {"id":2,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Sink"},"params":{"Props":[{"mute":true}]}}},
  {"id":3,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Audio/Source"},"params":{"Props":[{"mute":true}]}}},
  {"id":4,"type":"PipeWire:Interface:Node","info":{"props":{"media.class":"Stream/Output/Audio"},"params":{"Props":[{"mute":false}]}}}
]
EOF
cat >"$work/pw-dump" <<'EOF'
#!/usr/bin/env bash
cat "$AUDIO_TEST_NODES"
EOF
cat >"$work/wpctl" <<'EOF'
#!/usr/bin/env bash
if [[ $1 == get-volume ]]; then
  if [[ ${AUDIO_TEST_ALL_MUTED:-0} == 1 ]]; then
    echo 'Volume: 0.20 [MUTED]'
    exit 0
  fi
  case $2 in
  1) echo 'Volume: 0.40' ;;
  *) echo 'Volume: 0.20 [MUTED]' ;;
  esac
  exit 0
fi
printf '%s\n' "$*" >> "$AUDIO_TEST_LOG"
EOF
chmod +x "$work/pw-dump" "$work/wpctl"
export PATH="$work:$PATH"

bash "$script" output volume 5%-
printf 'set-volume -l 1 1 5%%-\nset-volume -l 1 2 5%%-\n' >"$work/expected"
cmp "$AUDIO_TEST_LOG" "$work/expected"
: >"$AUDIO_TEST_LOG"
bash "$script" output mute toggle
printf 'set-mute 1 1\nset-mute 2 1\n' >"$work/expected"
cmp "$AUDIO_TEST_LOG" "$work/expected"
: >"$AUDIO_TEST_LOG"
bash "$script" input mute toggle
printf 'set-mute 3 0\n' >"$work/expected"
cmp "$AUDIO_TEST_LOG" "$work/expected"
: >"$AUDIO_TEST_LOG"
AUDIO_TEST_ALL_MUTED=1 bash "$script" output mute toggle
printf 'set-mute 1 0\nset-mute 2 0\n' >"$work/expected"
cmp "$AUDIO_TEST_LOG" "$work/expected"
if bash "$script" output volume invalid; then
  echo 'FAIL: invalid volume accepted' >&2
  exit 1
fi
bash "$script" state | jq -e '.output.count == 2 and .output.mixed and (.output.muted | not) and .input.count == 1 and .input.muted' >/dev/null
echo '[]' >"$AUDIO_TEST_NODES"
bash "$script" state | jq -e '.output.count == 0 and .input.count == 0' >/dev/null
bash "$script" output mute toggle
echo 'PASS: device groups, hardware mute state, no app changes, no-device state'
