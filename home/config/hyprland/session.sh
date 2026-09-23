# shellcheck shell=bash
# Session actions for the Hyprland menu. Destructive menu rows require the
# explicit confirm choice. Display off is DPMS, not the COSMIC output toggle.
# The menu file is label, action, confirm, confirmText separated by tabs.
# Actions are a fixed set; the file cannot supply a shell command.

confirm() {
  action="$1"
  no="${HYPR_SESSION_CONFIRM_NO:-No}"
  prefix="${HYPR_SESSION_CONFIRM_YES:-Yes,}"
  yes="${prefix} ${action}"
  choice=$(
    printf '%s\n' "$no" "$yes" |
      fuzzel --dmenu --config "$HYPR_SESSION_FUZZEL_CONFIG" --prompt "Confirm ${action}? "
  ) || return 1
  [ "$choice" = "$yes" ]
}

run_action() {
  case "$1" in
  lock) exec hyprlock ;;
  display-off) exec hyprctl dispatch dpms off ;;
  display-toggle) exec hyprctl dispatch dpms toggle ;;
  suspend) exec systemctl suspend ;;
  # A Waybar-launched helper shares its service cgroup. Let compositor exit
  # trigger cleanup, rather than killing this helper before it can dispatch.
  logout) exec hyprctl dispatch exit ;;
  reboot) exec systemctl reboot ;;
  poweroff) exec systemctl poweroff ;;
  *)
    printf 'hypr-session: unknown action\n' >&2
    exit 1
    ;;
  esac
}

menu() {
  file="${HYPR_SESSION_MENU_FILE:?hypr-session: menu file is not set}"
  prompt="${HYPR_SESSION_PROMPT:-Session > }"
  choice=$(
    cut -f1 "$file" |
      fuzzel --dmenu --config "$HYPR_SESSION_FUZZEL_CONFIG" --prompt "$prompt"
  ) || exit 0
  [ -n "$choice" ] || exit 0
  line=$(
    awk -F '\t' -v choice="$choice" '
      $1 == choice { print; found = 1; exit }
      END { if (!found) exit 1 }
    ' "$file"
  ) || {
    printf 'hypr-session: unknown choice\n' >&2
    exit 1
  }
  action=$(printf '%s\n' "$line" | cut -f2)
  needs_confirm=$(printf '%s\n' "$line" | cut -f3)
  confirm_text=$(printf '%s\n' "$line" | cut -f4-)
  if [ "$needs_confirm" = 1 ]; then
    confirm "$confirm_text" || exit 0
  fi
  run_action "$action"
}

case "${1:-menu}" in
lock | display-off | display-toggle | suspend | logout | reboot | poweroff)
  run_action "$1"
  ;;
menu) menu ;;
*)
  printf 'usage: hypr-session lock|display-off|display-toggle|suspend|logout|reboot|poweroff|menu\n' >&2
  exit 64
  ;;
esac
