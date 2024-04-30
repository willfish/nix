#!/usr/bin/env bash

# Define options
options="Lock\nHibernate\nLogout\nShutdown\nSuspend\nReboot\nCancel"

# Prompt the user to select an option using wofi
choice=$(echo -e "$options" | wofi --dmenu --insensitive --hide-scroll --prompt "Power Menu:" --style ~/.dotfiles/home/config/wofi/style.css)

# Execute the chosen action
case "$choice" in
    "Lock")
        swaylock
        ;;
    "Hibernate")
        systemctl hibernate
        ;;
    "Logout")
        sleep 1; hyprctl dispatch exit
        ;;
    "Shutdown")
        systemctl poweroff
        ;;
    "Suspend")
        systemctl suspend
        ;;
    "Reboot")
        systemctl reboot
        ;;
    "Cancel")
        ;;
    *)
        ;;
esac
