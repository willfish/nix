# User-facing Hyprland session settings. The theme renderer imports this file
# and reads appearance.font, monoFont, fontSize, rounding and borderSize.
# Leave palette null to follow the host's default theme-menu selection.
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
rec {
  appearance = {
    # null uses the host default; otherwise use a theme ID such as "nord".
    # A saved theme-menu selection takes precedence. Each theme owns its mode.
    palette = null;
    # Boot artwork is embedded in the initrd: rebuild the system to change it.
    # null uses the host default; unlike palette, menu selections cannot win.
    bootPalette = null;
    # Omarchy defaults: Liberation Sans/Serif for proportional text,
    # JetBrainsMono Nerd Font for terminal, bar, and monospace.
    # Size stays 12. Omarchy's terminal default is 9pt; its shell base is 12px.
    font = "Liberation Sans";
    serifFont = "Liberation Serif";
    monoFont = "JetBrainsMono Nerd Font";
    fontSize = 12;
    rounding = 8;
    borderSize = 2;
    gapsIn = 5;
    gapsOut = 8;
    floatGaps = 8;
    activeOpacity = 0.92;
    inactiveOpacity = 0.87;
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

  # All upstream default themes include their matching wallpaper.
  wallpaper = {
    mode = "fill";
    # Rotate the picked theme's backgrounds. 0 keeps the first image.
    intervalMinutes = 5;
    # Optional absolute image paths by palette ID and mode, e.g.
    # overrides.tokyo-night.dark = "/home/william/Pictures/wallpaper.png";
    # An override is one image, so it does not rotate.
    overrides = { };
  };

  # Omarchy does not name a pointer theme. Arch default-cursors inherits
  # Adwaita, and default/hypr/envs.lua sets both cursor sizes to 24.
  # looknfeel.lua hides the pointer while typing and warps it on workspace change.
  cursor = {
    theme = "Adwaita";
    size = 24;
    hideOnKeyPress = true;
    warpOnChangeWorkspace = 1;
  };

  window = {
    layout = "dwindle";
    preserveSplit = true;
    # Hyprland 0.55 has no cursor-follows-focus.
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

  # Menus share launcher placement and the active theme's fonts/colours.
  menus.voice = {
    width = 55;
    lines = 10;
  };

  bar = {
    position = "left";
    width = 48;
    opacity = 0.92;
    traySpacing = 12;
    sessionLabel = "⏻";
    commands = {
      launcher = "hypr-launcher";
      files = "nautilus --new-window";
      audio = "hypr-controls audio";
      network = "hypr-controls network";
      bluetooth = "hypr-controls bluetooth";
      tailscale = "hypr-controls tailscale";
      calendar = "hypr-controls calendar";
      weather = "hypr-controls weather";
      arxiv = "hypr-controls arxiv";
      agents = "hypr-controls agents";
      notifications = "hypr-notifications";
      music = cliampCommand;
      power = "hypr-session menu";
    };
    modulesLeft = [
      "custom/launcher"
      "custom/agents"
      "custom/recording"
      "hyprland/workspaces"
    ];
    modulesCenter = [ "clock" ];
    modulesRight = [
      "tray"
      "mpris"
      "pulseaudio"
      "pulseaudio#microphone"
      "network"
      "custom/tailscale"
      "bluetooth"
      "custom/weather"
      "custom/arxiv"
      "idle_inhibitor"
      "battery"
      "custom/notifications"
      "custom/session"
    ];
    # Native Waybar options override individual widget defaults.
    widgets = { };
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

  # New windows, not already-open ones. Super shortcuts still focus an app wherever it is.
  # Web apps stay floating; only the main Brave window is the browser.
  # YouTube is the app-mode window and opens on workspace 3, not with Brave.
  workspace.rules = [
    {
      name = "browser";
      class = "^brave-browser$";
      workspace = 1;
    }
    {
      name = "slack";
      class = "^(slack|Slack)$";
      workspace = 2;
    }
    {
      name = "telegram";
      class = "^(org\\.telegram\\.desktop|TelegramDesktop)$";
      workspace = 2;
    }
    {
      name = "discord";
      class = "^(discord|Discord)$";
      workspace = 2;
    }
    {
      name = "youtube";
      class = "^brave-www[.]youtube[.]com.*";
      workspace = 3;
    }
    {
      name = "cliamp";
      class = "^com\\.william\\.cliamp$";
      workspace = 3;
    }
    {
      name = "spotify";
      title = "^Omarchy Spotify$";
      workspace = 3;
    }
  ];

  idle = {
    # Omarchy's idle order: animated wordmark, then lock, then display off.
    screensaverSeconds = 150;
    lockSeconds = 600;
    displayOffSeconds = 900;
  };

  notifications = {
    timeoutMs = 5000;
    width = 360;
    anchor = "top-right";
  };

  # Own window class so Super+P can focus CLIamp without focusing Ghostty.
  # The title match covers a player opened before that class existed.
  # Bare cliamp still starts on its three built-in streams.
  cliampCommand = "hypr-open com.william.cliamp ghostty --class=com.william.cliamp -e cliamp-radio";

  # Shortcuts: WASD focus, Super+X launcher, Super+K keybindings,
  # Super+Shift+T theme menu. Super+V records a region; press it again to stop.
  # Super+Alt+V does the same with a camera square and the microphone.
  # Ctrl+Super+V records only the camera and microphone, for a video message.
  # Super+Shift+B toggles output power through DPMS.
  # Super+Escape locks without a confirmation. The session menu confirms
  # logout, reboot and power off.
  # Daily apps: Super+Enter opens a new Ghostty. Hyprland names that
  # key Return. Super+B, Super+C, Super+T, Super+E, Super+P and Super+O
  # focus Brave, Slack, Telegram, Nautilus, CLIamp or Spotify, or launch
  # them when they are not open. A second press focuses the open window
  # from any workspace. Discord and LibreOffice stay on the launcher.
  # Spotify is the Quickshell player, not the official desktop client.
  # bindd lines are described keybinds. Super+K reads those descriptions.
  # mainMod is $mainMod. Number keys use workspaceMod and workspaceMoveMod;
  # they are not hardcoded in the module.
  bindings = {
    mainMod = "SUPER";
    workspaceMod = "SUPER";
    workspaceMoveMod = "SUPER SHIFT";
    bindd = [
      "SUPER, K, Keybindings, exec, omarchy-menu-keybindings"
      "SUPER, Q, Close window, killactive,"
      "SUPER, F, Full screen, fullscreen, 0"
      "SUPER, G, Toggle window floating, togglefloating,"
      "SUPER SHIFT, G, Toggle agent usage, exec, hypr-controls agents"
      "SUPER, W, Focus on above window, movefocus, u"
      "SUPER, A, Focus on left window, movefocus, l"
      "SUPER, S, Focus on below window, movefocus, d"
      "SUPER, D, Focus on right window, movefocus, r"
      "SUPER SHIFT, W, Move window up, movewindow, u"
      "SUPER SHIFT, A, Move window left, movewindow, l"
      "SUPER SHIFT, S, Move window down, movewindow, d"
      "SUPER SHIFT, D, Move window right, movewindow, r"
      "SUPER, X, Launch apps, exec, hypr-launcher"
      "SUPER, Return, Terminal, exec, ghostty"
      "SUPER, B, Browser, exec, hypr-open brave-browser brave"
      "SUPER, C, Slack, exec, hypr-open slack:Slack slack"
      "SUPER, T, Telegram, exec, hypr-open org.telegram.desktop:TelegramDesktop Telegram"
      "SUPER, E, File manager, exec, hypr-open org.gnome.Nautilus nautilus --new-window"
      "SUPER CTRL, E, Emojis, exec, hypr-emojis"
      "SUPER, P, CLIamp, exec, ${cliampCommand}"
      "SUPER, O, Spotify, exec, hypr-spotify-focus"
      "SUPER SHIFT, T, Theme menu, exec, theme-menu"
      "SUPER SHIFT, left, Previous wallpaper, exec, hypr-wallpaper-cycle previous"
      "SUPER SHIFT, right, Next wallpaper, exec, hypr-wallpaper-cycle next"
      "SUPER SHIFT, B, Toggle display power, exec, hypr-session display-toggle"
      "SUPER, Escape, Lock system, exec, hypr-session lock"
      "SUPER SHIFT, Escape, System menu, exec, hypr-session menu"
      "CTRL SHIFT, S, Screenshot, exec, grimblast save area - | satty --filename -"
      ", Print, Screenshot, exec, grimblast save area - | satty --filename -"
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
    # Opens the top-of-monitor card and records into the selected Pi session.
    interact = "SUPER, space, Voice, exec, pi-voice-interact";
    send = "SUPER SHIFT, space, Send voice, exec, pi-voice send";
    menu = "SUPER SHIFT, V, Voice picker, exec, voice-menu";
    read = "SUPER, R, Read aloud, exec, pi-voice read";
  };
}
