# shellcheck shell=bash
panel=${1:-}
action=${2:-open}
case "$panel" in
audio | bluetooth | network | tailscale) ;;
*)
  echo 'Usage: hypr-controls audio|bluetooth|network|tailscale [toggle]' >&2
  exit 2
  ;;
esac
if [[ $action != open && ! ($panel == tailscale && $action == toggle) ]]; then
  echo 'Usage: hypr-controls audio|bluetooth|network|tailscale [toggle]' >&2
  exit 2
fi
systemctl --user is-active --quiet hyprland-session.target || {
  echo 'Desktop controls require an active Hyprland session.' >&2
  exit 1
}
systemctl --user start hyprland-panels.service
cursor=$(hyprctl -j cursorpos)
mapfile -t anchor < <(hyprctl -j monitors | jq -r --argjson p "$cursor" '
  . as $monitors |
  [.[] |
    ((if .transform % 2 == 0 then .width else .height end) / .scale) as $w |
    ((if .transform % 2 == 0 then .height else .width end) / .scale) as $h |
    select($p.x >= .x and $p.x < .x + $w and
           $p.y >= .y and $p.y < .y + $h)] |
  (.[0] // ($monitors | map(select(.focused))[0]) // $monitors[0]) |
  .name, ($p.x - .x), ($p.y - .y)')
# Wait for registration, then issue the toggle exactly once.
for _ in {1..30}; do
  if quickshell ipc --path "$HYPR_CONTROLS_CONFIG" show 2>/dev/null |
    grep -q 'controls'; then
    if [[ $action == toggle ]]; then
      exec quickshell ipc --path "$HYPR_CONTROLS_CONFIG" call omarchy.tailscale toggleTailscale
    fi
    exec quickshell ipc --path "$HYPR_CONTROLS_CONFIG" call controls toggle \
      "$panel" "${anchor[@]}"
  fi
  sleep 0.1
done
notify-send 'Desktop controls' 'The panel service could not start.'
exit 1
