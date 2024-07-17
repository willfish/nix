#!/usr/bin/env bash

# Define options
options="Suspend\nReboot\nLock\nHibernate\nShutdown\nCancel"

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
