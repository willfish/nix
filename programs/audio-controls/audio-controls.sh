#!/usr/bin/env bash
set -euo pipefail

# Hardware routes can differ from raw node Props; use WirePlumber's view.
if [[ ${1:-} == state ]]; then
  nodes=$(pw-dump | jq -r '.[] | select(.type == "PipeWire:Interface:Node") |
    select(.info.props["media.class"] == "Audio/Sink" or .info.props["media.class"] == "Audio/Source") |
    [.id, .info.props["media.class"]] | @tsv')
  while read -r id class; do
    [[ -n $id ]] || continue
    status=$(wpctl get-volume "$id")
    read -r _ volume muted <<<"$status"
    jq -nc --arg class "$class" --argjson volume "$volume" --arg muted "${muted:-}" \
      '{class:$class, volume:$volume, muted:($muted == "[MUTED]")}'
  done <<<"$nodes" | jq -s '
    def group($class): [.[] | select(.class == $class)] |
      {count:length, volume:([.[].volume] | max // 0), muted:all(.muted),
       mixed:(if length > 1 then (([.[].volume] | max) - ([.[].volume] | min) > 0.005) else false end)};
    {output:group("Audio/Sink"), input:group("Audio/Source")}'
  exit 0
fi

case ${1:-} in
output) class=Audio/Sink ;;
input) class=Audio/Source ;;
*)
  echo 'Usage: hypr-audio-controls output|input volume|mute VALUE' >&2
  exit 2
  ;;
esac
action=${2:-}
value=${3:-}
case $action in
volume)
  [[ $value =~ ^[0-9]+([.][0-9]+)?%?[+-]?$ ]] || exit 2
  ;;
mute) [[ $value == 0 || $value == 1 || $value == toggle ]] || exit 2 ;;
*) exit 2 ;;
esac

# Device nodes only: never overwrite application-stream volume or mute state.
nodes=$(pw-dump | jq -r --arg class "$class" '.[] |
  select(.type == "PipeWire:Interface:Node" and .info.props["media.class"] == $class) | .id')
mapfile -t ids < <(printf '%s' "$nodes")
if [[ $action == mute && $value == toggle ]]; then
  # Mixed mute states must converge, not swap which devices are audible.
  value=0
  for id in "${ids[@]}"; do
    status=$(wpctl get-volume "$id")
    if [[ $status != *'[MUTED]'* ]]; then
      value=1
      break
    fi
  done
fi
for id in "${ids[@]}"; do
  case $action in
  volume) wpctl set-volume -l 1 "$id" "$value" ;;
  mute) wpctl set-mute "$id" "$value" ;;
  esac
done
