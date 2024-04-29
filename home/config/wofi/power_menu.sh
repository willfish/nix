#!/usr/bin/env bash

# Define options
options="Shutdown\nSuspend\nLogout\nReboot\nCancel"

# Prompt the user to select an option using wofi
choice=$(echo -e "$options" | wofi --dmenu --insensitive --hide-scroll --prompt "Power Menu:" --style ~/.dotfiles/home/config/wofi/style.css)

# Execute the chosen action
case "$choice" in
    "Shutdown") systemctl poweroff ;;
    "Reboot") systemctl reboot ;;
    "Suspend") systemctl suspend ;;
    "Logout") pkill -KILL -u $USER ;;
    "Cancel") exit 0 ;;
    *) ;;
esac
