# shellcheck shell=bash
# Session actions for the Hyprland menu and searchable launcher rows.
# Menu/select use the configured confirmation policy. Direct CLI actions retain
# their immediate behaviour. Display off uses DPMS.
# The menu file is label, action, confirm, confirmText separated by tabs.
# Actions are a fixed set; the file cannot supply a shell command.

confirm() {
  local action="$1" no prefix yes choice
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
  # Explicit menu suspend overrides a working-agent idle/sleep inhibitor.
  suspend) exec systemctl suspend --ignore-inhibitors ;;
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

run_entry() {
  local line="$1" action needs_confirm confirm_text
  action=$(printf '%s\n' "$line" | cut -f2)
  needs_confirm=$(printf '%s\n' "$line" | cut -f3)
  confirm_text=$(printf '%s\n' "$line" | cut -f4-)
  case "$needs_confirm" in
  0) ;;
  1) confirm "$confirm_text" || return 0 ;;
  *)
    printf 'hypr-session: invalid confirmation policy\n' >&2
    return 1
    ;;
  esac
  run_action "$action"
}

select_action() {
  local action="$1" file line
  case "$action" in
  lock | display-off | display-toggle | suspend | logout | reboot | poweroff) ;;
  *)
    printf 'hypr-session: unknown action\n' >&2
    return 1
    ;;
  esac
  file="${HYPR_SESSION_MENU_FILE:?hypr-session: menu file is not set}"
  line=$(
    awk -F '\t' -v action="$action" '
      $2 == action { row = $0; count++ }
      END { if (count != 1) exit 1; print row }
    ' "$file"
  ) || {
    printf 'hypr-session: unavailable or ambiguous action\n' >&2
    return 1
  }
  run_entry "$line"
}

menu() {
  local file prompt choice line
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
  run_entry "$line"
}

case "${1:-menu}" in
lock | display-off | display-toggle | suspend | logout | reboot | poweroff)
  run_action "$1"
  ;;
menu) menu ;;
select)
  if [ "$#" -ne 2 ]; then
    printf 'usage: hypr-session select ACTION\n' >&2
    exit 64
  fi
  select_action "$2"
  ;;
*)
  printf 'usage: hypr-session lock|display-off|display-toggle|suspend|logout|reboot|poweroff|menu|select ACTION\n' >&2
  exit 64
  ;;
esac
