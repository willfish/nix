#!/usr/bin/env bash

# Terminate already running bar instances
# If all your bars have ipc enabled, you can use
polybar-msg cmd quit
# Otherwise you can use the nuclear option:
# killall -q polybar

fetch_window_manager() {
  loginctl show-session $XDG_SESSION_ID | grep Desktop | cut -d= -f2 | sed 's/none+//'
}

window_manager=$(fetch_window_manager)
bar="mainbar-i3"
case $(fetch_window_manager) in
  "i3")
    bar="mainbar-i3"
    ;;
  "xmonad")
    bar="mainbar-xmonad"
    ;;
  *)
    bar="main"
    ;;
esac

echo "---" > /tmp/polybar.log
if type "xrandr"; then
  for m in $(xrandr --query | grep " connected" | cut -d" " -f1); do
    MONITOR=$m polybar --reload $bar & disown
  done
else
  polybar --reload $bar & disown
fi
# polybar example 2>&1 | tee -a /tmp/polybar.log & disown
