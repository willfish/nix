import XMonad
import XMonad.Hooks.ManageDocks
import XMonad.Hooks.DynamicLog
import XMonad.Util.EZConfig (additionalKeysP)
import XMonad.Util.SpawnOnce
import XMonad.Layout.Spacing
import XMonad.Layout.NoBorders
import XMonad.Layout.Gaps
import XMonad.StackSet as W
import XMonad.Hooks.ManageHelpers (isDialog, doCenterFloat)

main = xmonad $ docks def {
    terminal           = "ghostty",
    modMask            = mod4Mask, -- Use the Super key as mod
    layoutHook         = myLayoutHook,
    manageHook         = myManageHook,
    handleEventHook    = handleEventHook def <+> docksEventHook,
    startupHook        = myStartupHook,
    borderWidth        = 2,
    normalBorderColor  = "#1ABC9C",
    focusedBorderColor = "#CB4B16",
    workspaces         = myWorkspaces
  } `additionalKeysP` myKeybindings

-- Define Workspaces
myWorkspaces = ["1", "2", "3", "4", "5", "6", "7", "8"]

-- Layouts
myLayoutHook = gaps [(L,5), (R,5), (U,5), (D,5)]
              $ spacing 5
              $ smartBorders
              $ layoutHook def

-- Manage Hooks (Floating Windows)
myManageHook = composeAll
  [ className =? "Pavucontrol"        --> doCenterFloat
  , className =? "Nitrogen"           --> doCenterFloat
  , isDialog                          --> doCenterFloat
  , title     =? "alsamixer"          --> doCenterFloat
  , title     =? "File Transfer*"     --> doCenterFloat
  , manageDocks
  ]

-- Startup Applications
myStartupHook = do
  spawnOnce "picom &" -- Compositor
  spawnOnce "nitrogen --restore &" -- Wallpaper
  spawnOnce "nm-applet &" -- Network Manager applet
  spawnOnce "blueman-applet &" -- Bluetooth Manager
  -- spawnOnce "xautolock -time 10 -locker i3lock --color=282828 -detectsleep &"
  spawnOnce "~/.config/polybar/launch.sh &" -- Polybar
  spawnOnce "~/.bin/random-wallpaper &"

-- Keybindings
myKeybindings = [
    -- Applications
    ("M-<Return>", spawn "ghostty"), -- Terminal
    ("M-x", spawn "rofi -show combi -combi-modes 'run,ssh' -show-icons -display-run '' -modes combi -theme macos"), -- Rofi
    ("M-l", spawn "~/.dotfiles/home/config/rofi/power_menu.sh"), -- Power Menu
    ("M-t", spawn "pkill picom"), -- Kill Picom
    ("M-C-t", spawn "picom -b"), -- Restart Picom
    ("C-S-s", spawn "maim -s -u | tee ~/Pictures/screenshot-$(date +%Y-%m-%d_%H-%M-%S).png | swappy -f -"), -- Screenshot with Swappy
    ("M-S-d", spawn "killall dunst; notify-send 'restart dunst'"), -- Restart Dunst

    -- Focus movement
    ("M-a", windows W.focusMaster), -- Focus master window
    ("M-s", windows W.focusDown),   -- Focus next window
    ("M-w", windows W.focusUp),     -- Focus previous window
    ("M-d", windows W.focusDown),   -- Focus next window

    -- Move windows
    ("M-S-a", windows W.swapMaster), -- Swap with master
    ("M-S-s", windows W.swapDown),   -- Swap with next
    ("M-S-w", windows W.swapUp),     -- Swap with previous

    -- Workspaces
    ("M-" ++ show i, windows $ W.view ws) -- Switch workspace
    | (i, ws) <- zip [1..8] (myWorkspaces)
  ] ++ [
    ("M-S-" ++ show i, windows $ W.shift ws) -- Move window to workspace
    | (i, ws) <- zip [1..8] (myWorkspaces)
  ]
