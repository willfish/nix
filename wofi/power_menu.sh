#!/usr/bin/env bash

# Define options
options="😴 Shutdown\n💤 Suspend\n❌ Logout\n🟢 Cancel"

# Prompt the user to select an option using wofi
choice=$(echo -e "$options" | wofi --dmenu --insensitive --hide-scroll --prompt "Power Menu:" --style ~/.dotfiles/wofi/dracula.css)

# Execute the chosen action
case "$choice" in
    "😴 Shutdown") systemctl poweroff ;;
    "Suspend") systemctl suspend ;;
    "Logout") pkill -KILL -u $USER ;;
    "Cancel") exit 0 ;;
    *) ;;
esac
