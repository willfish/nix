import XMonad
import XMonad.Operations (restart)

import XMonad.Hooks.ManageDocks (docks, avoidStruts, manageDocks)
import XMonad.Hooks.DynamicLog
import XMonad.Util.EZConfig (additionalKeysP)
import XMonad.Util.SpawnOnce
import XMonad.Layout.Spacing (spacing)
import XMonad.Layout.NoBorders (smartBorders)
import XMonad.Layout.ToggleLayouts (toggleLayouts, ToggleLayout(Toggle))
import XMonad.Layout.Gaps
import qualified XMonad.StackSet as W
import XMonad.Hooks.ManageHelpers (isDialog, doCenterFloat)

main = xmonad . docks $ def {
    terminal           = "ghostty",
    modMask            = mod4Mask, -- Use the Super key as mod
    layoutHook         = myLayoutHook,
    manageHook         = myManageHook <+> manageDocks,
    startupHook        = myStartupHook,
    borderWidth        = 2,
    normalBorderColor  = "#1ABC9C",
    focusedBorderColor = "#CB4B16",
    workspaces         = myWorkspaceNames -- Explicitly assign XConfig.workspaces
  } `additionalKeysP` myKeybindings

myWorkspaceNames :: [String]
myWorkspaceNames = ["1", "2", "3", "4", "5", "6", "7", "8"]

myLayoutHook =
  avoidStruts $
  spacing 5 $
  smartBorders $
  layoutHook def

myManageHook = composeAll
  [ isDialog           --> doCenterFloat
  , className =? "Pavucontrol" --> doCenterFloat
  , className =? "Nitrogen"    --> doCenterFloat
  , isDialog           --> doCenterFloat
  , title     =? "alsamixer"    --> doCenterFloat
  , title     =? "File Transfer*"  --> doCenterFloat
  , manageDocks
  ]

myStartupHook = do
  spawnOnce "picom &"
  spawnOnce "nm-applet &"
  spawnOnce "pa-applet &"
  spawnOnce "blueman-applet &"
  spawnOnce "~/.config/polybar/launch.sh &"
  spawnOnce "~/.bin/random-wallpaper &"

myKeybindings =
  [ -- Applications
    ("M-<Return>", spawn "ghostty")
  , ("M-q", kill)
  , ("M-x", spawn "rofi -show combi -combi-modes 'run,ssh' -show-icons -display-run '' -modes combi -theme macos")
  , ("M-l", spawn "~/.dotfiles/home/config/rofi/power_menu.sh")
  , ("M-t", spawn "pkill picom")
  , ("M-C-t", spawn "picom -b")
  , ("C-S-s", spawn "maim -s -u | tee ~/Pictures/screenshot-$(date +%Y-%m-%d_%H-%M-%S).png | swappy -f -")
  , ("M-S-d", spawn "killall dunst; notify-send 'restart dunst'")

    -- Focus movement
  , ("M-a", windows W.focusMaster)
  , ("M-s", windows W.focusDown)
  , ("M-w", windows W.focusUp)
  , ("M-d", windows W.focusDown)

    -- Move windows
  , ("M-S-a", windows W.swapMaster)
  , ("M-S-s", windows W.swapDown)
  , ("M-S-w", windows W.swapUp)
  , ("M-S-d", windows W.swapDown)

    -- Reload/Recompile XMonad
  , ("M-S-r", restart "/run/current-system/sw/bin/xmonad" True)

    -- Fullscreen focused window
  , ("M-f", sendMessage (Toggle "Full"))
  ]
  ++
  [ -- Workspace navigation
    ("M-" ++ show i, windows $ W.view ws)
    | (i, ws) <- zip [1..8] myWorkspaceNames
  ]
  ++
  [ -- Move window to workspace
    ("M-S-" ++ show i, windows $ W.shift ws)
    | (i, ws) <- zip [1..8] myWorkspaceNames
  ]
