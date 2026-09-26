#!/usr/bin/env bash
# One fullscreen Ghostty per monitor. Spawn is async, so wait for each
# window before moving focus or every terminal lands on the last monitor.
set -euo pipefail

class=org.omarchy.screensaver
state="${XDG_STATE_HOME:-$HOME/.local/state}/dotfiles/screensaver-off"

if pgrep -f '[o]rg.omarchy.screensaver' >/dev/null 2>&1; then
  exit 0
fi

if [[ ${1:-} != force && -f $state ]]; then
  exit 0
fi

if [[ -z ${HYPRLAND_INSTANCE_SIGNATURE:-} || -z ${XDG_RUNTIME_DIR:-} ]]; then
  echo "screensaver needs a Hyprland session" >&2
  exit 1
fi

ghostty=$(command -v ghostty)
runner=$(command -v hypr-screensaver)
config="${GHOSTTY_SCREENSAVER_CONFIG:?}"
socket="$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock"
focused=$(hyprctl monitors -j | jq -r '.[] | select(.focused == true) | .name')

exec {events}< <(socat -U - "UNIX-CONNECT:$socket")

wait_for_window() {
  local line deadline=$((SECONDS + 5))
  while ((SECONDS < deadline)) && IFS= read -r -t $((deadline - SECONDS)) -u "$events" line; do
    [[ $line == openwindow\>\>*,"$class",* ]] && return 0
  done
}

focus_monitor() {
  hyprctl dispatch "hl.dsp.focus({ monitor = \"$1\" })" >/dev/null 2>&1 ||
    hyprctl dispatch focusmonitor "$1" >/dev/null
}

for monitor in $(hyprctl monitors -j | jq -r '.[].name'); do
  focus_monitor "$monitor"
  # This Hyprland rejects hl.dsp.exec_cmd but still exits 0, so use exec.
  hyprctl dispatch exec -- "$ghostty" --class="$class" --gtk-single-instance=false --config-file="$config" -e "$runner"
  wait_for_window
done

if [[ -n $focused ]]; then
  focus_monitor "$focused"
fi
