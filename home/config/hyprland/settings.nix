# User-facing Hyprland session settings. The theme renderer imports this file
# and reads appearance.font, monoFont, fontSize, rounding and borderSize.
# Leave palette and mode null to follow the host theme-menu selection.
# paletteOverrides are six-digit Base16 tokens, no hash, merged onto every
# named palette before desktop assets are rendered.
#
# Theme-menu publishes, and this session sources or includes:
#   ~/.local/state/theme-menu/active/hyprland.conf
#   ~/.local/state/theme-menu/active/fuzzel.ini
#   ~/.local/state/theme-menu/active/waybar.css
#   ~/.local/state/theme-menu/active/hyprlock.conf
# hyprlock.conf must be variables only ($theme_*). Repeat categories add
# widgets; the main config owns every lock-screen widget.
{
  appearance = {
    # null uses the host catalogue default. A value is a host key
    # (andromeda) or a herdr palette id (rose-pine). appearance.nix owns
    # applying this as the theme-menu default.
    palette = null;
    # null follows COSMIC is_dark. "light" or "dark" is an explicit preference.
    mode = null;
    font = "Ubuntu";
    monoFont = "JetBrainsMono Nerd Font";
    fontSize = 12;
    rounding = 8;
    borderSize = 2;
    gapsIn = 5;
    gapsOut = 8;
    floatGaps = 8;
    activeOpacity = 1.0;
    inactiveOpacity = 1.0;
    blur = true;
    shadow = true;
    shadowRange = 4;
    # Null keeps the sourced theme colour. A value is a Hyprland colour.
    colors = {
      activeBorder = null;
      inactiveBorder = null;
      shadow = null;
      shadowInactive = null;
    };
    # Applied to the selected palette for both the fallback renderer and,
    # via appearance.nix, every runtime named palette.
    paletteOverrides = {
      light = { };
      dark = { };
    };
  };

  window = {
    layout = "dwindle";
    preserveSplit = true;
    # COSMIC focus_follows_cursor. Hyprland 0.55 has no cursor-follows-focus.
    focusFollowsCursor = true;
    resizeOnBorder = true;
    naturalScroll = true;
    keyboardLayout = "us";
  };

  launcher = {
    width = 40;
    lines = 12;
    anchor = "center";
    terminal = "ghostty";
    matchMode = "fzf";
    layer = "overlay";
  };

  bar = {
    position = "left";
    width = 48;
    sessionLabel = "PWR";
    commands = {
      audio = "cosmic-settings sound";
      network = "cosmic-settings network";
      bluetooth = "cosmic-settings bluetooth";
      power = "cosmic-settings power";
    };
    modulesLeft = [ "hyprland/workspaces" ];
    modulesCenter = [ "clock" ];
    modulesRight = [
      "tray"
      "pulseaudio"
      "network"
      "bluetooth"
      "battery"
      "custom/session"
    ];
  };

  workspaces = 9;

  # null keeps ~/Pictures/Screenshots.
  # Native Satty settings; capture and selection belong to Grimblast.
  screenshot = {
    directory = null;
    satty.general = {
      early-exit = false;
      copy-command = "wl-copy --type image/png";
      actions-on-enter = [
        "save-to-file"
        "save-to-clipboard"
        "exit"
      ];
      actions-on-escape = [ "exit" ];
      default-fill-shapes = true;
      corner-roundness = 0;
      floating-hack = true;
    };
  };

  # Menu rows only. Direct hypr-session lock|display-toggle do not confirm.
  # confirmText is the verb in "Yes, <confirmText>".
  session = {
    prompt = "Session > ";
    confirmNo = "No";
    confirmYes = "Yes,";
    entries = [
      {
        label = "Lock";
        action = "lock";
        confirm = false;
        confirmText = "lock";
      }
      {
        label = "Display off";
        action = "display-off";
        confirm = false;
        confirmText = "display off";
      }
      {
        label = "Suspend";
        action = "suspend";
        confirm = false;
        confirmText = "suspend";
      }
      {
        label = "Log out";
        action = "logout";
        confirm = true;
        confirmText = "log out";
      }
      {
        label = "Reboot";
        action = "reboot";
        confirm = true;
        confirmText = "reboot";
      }
      {
        label = "Power off";
        action = "poweroff";
        confirm = true;
        confirmText = "power off";
      }
    ];
  };

  floating.rules = [
    {
      name = "satty";
      class = "^com\\.gabm\\.satty$";
      float = true;
    }
  ];

  idle = {
    lockSeconds = 600;
    displayOffSeconds = 900;
  };

  notifications = {
    timeoutMs = 5000;
    width = 360;
    anchor = "top-right";
  };

  # Keys match the COSMIC map: WASD focus, Super+X launcher, Super+Shift+T
  # theme menu. Super+Shift+B is DPMS, not cosmic toggle-display.
  # Super+Escape locks without a confirmation. The session menu confirms
  # logout, reboot and power off.
  # bind lines are the keybinds. mainMod is $mainMod. Number keys use
  # workspaceMod and workspaceMoveMod; they are not hardcoded in the module.
  bindings = {
    mainMod = "SUPER";
    workspaceMod = "SUPER";
    workspaceMoveMod = "SUPER SHIFT";
    bind = [
      "SUPER, Q, killactive,"
      "SUPER, F, fullscreen, 0"
      "SUPER, G, togglefloating,"
      "SUPER, W, movefocus, u"
      "SUPER, A, movefocus, l"
      "SUPER, S, movefocus, d"
      "SUPER, D, movefocus, r"
      "SUPER SHIFT, W, movewindow, u"
      "SUPER SHIFT, A, movewindow, l"
      "SUPER SHIFT, S, movewindow, d"
      "SUPER SHIFT, D, movewindow, r"
      "SUPER, X, exec, hypr-launcher"
      "SUPER SHIFT, T, exec, theme-menu"
      "SUPER SHIFT, B, exec, hypr-session display-toggle"
      "SUPER, Escape, exec, hypr-session lock"
      "SUPER SHIFT, Escape, exec, hypr-session menu"
      "CTRL SHIFT, S, exec, grimblast save area - | satty --filename -"
      ", Print, exec, grimblast save area - | satty --filename -"
    ];
    bindle = [
      ", XF86AudioRaiseVolume, exec, wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+"
      ", XF86AudioLowerVolume, exec, wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"
      ", XF86AudioMute, exec, wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"
      ", XF86AudioMicMute, exec, wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle"
      ", XF86AudioPlay, exec, playerctl play-pause"
      ", XF86AudioPause, exec, playerctl play-pause"
      ", XF86AudioNext, exec, playerctl next"
      ", XF86AudioPrev, exec, playerctl previous"
      ", XF86MonBrightnessUp, exec, brightnessctl -e4 -n2 set 5%+"
      ", XF86MonBrightnessDown, exec, brightnessctl -e4 -n2 set 5%-"
    ];
    bindr = [ "SUPER, Super_L, exec, hypr-launcher" ];
    bindm = [
      "SUPER, mouse:272, movewindow"
      "SUPER, mouse:273, resizewindow"
    ];
  };

  # Included only when the matching voice capability is enabled.
  voice = {
    interact = "SUPER, space, exec, pi-voice interact";
    send = "SUPER SHIFT, space, exec, pi-voice send";
    menu = "SUPER SHIFT, V, exec, voice-menu";
    read = "SUPER, R, exec, pi-voice read";
  };
}
