#!/bin/bash

# Define options
options="Shutdown\nSuspend\nLogout"

# Prompt the user to select an option using wofi
choice=$(echo -e "$options" | wofi --dmenu --insensitive --hide-scroll --prompt "Power Menu:")

# Execute the chosen action
case "$choice" in
    "Shutdown") sudo shutdown now ;;
    "Suspend") systemctl suspend ;;
    "Logout") pkill -KILL -u $USER ;;
    *) ;;
esac
