#!/usr/bin/env bash

# Define options
options="Lock\nHibernate\nLogout\nShutdown\nSuspend\nReboot\nCancel"

# Prompt the user to select an option using rofi
choice=$(echo -e "$options" | rofi -dmenu -i -p "Power Menu:" -theme ~/.dotfiles/home/config/rofi/themes/macos.rasi)

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
