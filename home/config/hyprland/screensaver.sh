#!/usr/bin/env bash
# Idle canvas. ttfx measures the terminal once, so wait until Ghostty has
# left the default 80x24 pty before the first effect.
set -euo pipefail

branding="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/branding/screensaver.txt"

exit_screensaver() {
  hyprctl eval 'hl.config({ cursor = { invisible = false } })' >/dev/null 2>&1 ||
    hyprctl keyword cursor:invisible false >/dev/null 2>&1 ||
    true
  pkill -x ttfx >/dev/null 2>&1 || true
  pkill -f '[o]rg.omarchy.screensaver' >/dev/null 2>&1 || true
  exit 0
}

trap exit_screensaver SIGINT SIGTERM SIGHUP SIGQUIT

if [[ ! -f $branding ]]; then
  echo "screensaver branding is missing: $branding" >&2
  exit 1
fi

printf '\033]11;rgb:00/00/00\007'
hyprctl eval 'hl.config({ cursor = { invisible = true } })' >/dev/null 2>&1 ||
  hyprctl keyword cursor:invisible true >/dev/null 2>&1 ||
  true

tty_name=$(tty 2>/dev/null || true)
deadline=$((SECONDS + 2))
while ((SECONDS < deadline)) && [[ $(stty size 2>/dev/null || true) == "24 80" ]]; do
  sleep 0.02
done

while true; do
  ttfx -i "$branding" \
    --frame-rate 120 --canvas-width 0 --canvas-height 0 --reuse-canvas \
    --anchor-canvas c --anchor-text c \
    --random-effect --no-eol --no-restore-cursor &

  while [[ -n $tty_name ]] && pgrep -t "${tty_name#/dev/}" -x ttfx >/dev/null; do
    if read -r -n1 -t 1 || ! hyprctl activewindow -j | jq -e '.class == "org.omarchy.screensaver"' >/dev/null 2>&1; then
      exit_screensaver
    fi
  done
done
