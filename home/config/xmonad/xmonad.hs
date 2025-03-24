import XMonad
import XMonad.Hooks.DynamicLog
import XMonad.Hooks.EwmhDesktops (ewmh)
import XMonad.Hooks.ManageDocks (avoidStruts, docks, manageDocks)
import XMonad.Hooks.ManageHelpers (doCenterFloat, isDialog)
import XMonad.Layout.Gaps
import XMonad.Layout.MultiColumns (multiCol)
import XMonad.Layout.NoBorders (noBorders, smartBorders)
import XMonad.Layout.Spacing (spacing)
import XMonad.Layout.ToggleLayouts (ToggleLayout (Toggle), toggleLayouts)
import XMonad.Operations (restart)
import qualified XMonad.StackSet as W
import XMonad.Util.EZConfig (additionalKeysP)
import XMonad.Util.SpawnOnce

main :: IO ()
main =
  xmonad
    . ewmh
    . docks
    $ def
      { terminal = "ghostty",
        modMask = mod4Mask,
        layoutHook = myLayoutHook,
        manageHook = myManageHook <+> manageDocks,
        startupHook = myStartupHook,
        borderWidth = 2,
        normalBorderColor = "#1ABC9C",
        focusedBorderColor = "#CB4B16",
        workspaces = myWorkspaceNames
      }
      `additionalKeysP` myKeybindings

myWorkspaceNames :: [String]
myWorkspaceNames = ["web", "chat", "music", "video", "5", "6", "7", "8"]

myLayoutHook =
  avoidStruts $
    spacing 5 $
      smartBorders $
        myLayouts
          ( multiCol [1] 1 0.01 (-0.5)
              ||| layoutHook def
          )

myLayouts = toggleLayouts (noBorders Full)

myManageHook =
  composeAll
    [ className =? "Brave-browser" --> doShift "web",
      className =? "Clementine" --> doShift "music",
      className =? "Slack" --> doShift "chat",
      className =? "Spotify" --> doShift "music",
      className =? "brave-browser" --> doShift "web",
      className =? "clementine" --> doShift "music",
      className =? "discord" --> doShift "chat",
      className =? "spotify" --> doShift "music",
      className =? "zoom " --> doShift "video",
      className =? "zoom" --> doShift "video",
      className =? "Galculator" --> doCenterFloat,
      className =? "Simple-scan" --> doCenterFloat,
      className =? "zoom " --> doCenterFloat,
      className =? "zoom" --> doCenterFloat,
      className =? "Pavucontrol" --> doCenterFloat,
      className =? "gnome-mahjongg" --> doCenterFloat,
      title =? "File Transfer*" --> doCenterFloat,
      title =? "alsamixer" --> doCenterFloat,
      isDialog --> doCenterFloat,
      manageDocks
    ]

myStartupHook = do
  spawnOnce "xsetroot -cursor_name left_ptr &"
  spawnOnce "picom &"
  spawnOnce "nm-applet &"
  spawnOnce "pa-applet &"
  spawnOnce "blueman-applet &"
  spawnOnce "variety &"
  spawnOnce "~/.bin/bar &"

myKeybindings =
  [ -- Applications
    ("M-<Return>", spawn "ghostty"),
    ("M-q", kill),
    ("M-x", spawn "rofi -show combi -combi-modes 'run' -show-icons -display-run '' -modes combi -theme macos"),
    ("M-l", spawn "~/.bin/power"),
    ("M-b", spawn "~/.bin/bluetooth"),
    ("M-t", spawn "pkill picom"),
    ("M-C-t", spawn "picom -b"),
    ("C-S-s", spawn "maim -s -u | tee ~/Pictures/screenshot-$(date +%Y-%m-%d_%H-%M-%S).png | swappy -f -"),
    -- Focus movement
    ("M-a", windows W.focusMaster),
    ("M-s", windows W.focusDown),
    ("M-w", windows W.focusUp),
    ("M-d", windows W.focusDown),
    -- Move windows
    ("M-S-a", windows W.swapMaster),
    ("M-S-s", windows W.swapDown),
    ("M-S-w", windows W.swapUp),
    ("M-S-d", windows W.swapDown),
    -- Reload/Recompile XMonad
    ("M-S-r", restart "/run/current-system/sw/bin/xmonad" True),
    -- Fullscreen focused window
    ("M-f", sendMessage (Toggle "Full")),
    ("M-S-t", withFocused $ windows . W.sink)
  ]
    ++ [
         -- Workspace navigation
         ("M-" ++ show i, windows $ W.view ws)
         | (i, ws) <- zip [1 .. 8] myWorkspaceNames
       ]
    ++ [
         -- Move window to workspace
         ("M-S-" ++ show i, windows $ W.shift ws)
         | (i, ws) <- zip [1 .. 8] myWorkspaceNames
       ]
