#!/usr/bin/env bash

# Define options
options="Suspend\nReboot\nLock\nHibernate\nShutdown\nLogout\nCancel"

# Prompt the user to select an option using rofi
choice=$(echo -e "$options" | rofi -dmenu -i -p "Power Menu:" -theme ~/.dotfiles/home/config/rofi/themes/macos.rasi)

current_window_manager=$(loginctl show-session $XDG_SESSION_ID | grep Desktop | cut -d= -f2 | sed 's/none+//')

# Execute the chosen action
case "$choice" in
    "Lock")
        i3lock --color=282828
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
    "Logout")
        case $current_window_manager in
            "i3")
                i3-msg exit
                ;;
            "xmonad")
                pkill xmonad
                ;;
            *)
                loginctl terminate-session "$XDG_SESSION_ID"
                ;;
        ;;
    *)
        ;;
esac
