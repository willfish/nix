{
  config,
  lib,
  pkgs,
  hostName,
  hostTheme,
  isGraphicalLinux,
  ...
}:
{
  imports = [
    ./hyprland-panels.nix
    ./launcher.nix
    ./omarchy-spotify.nix
    ./omapager.nix
  ];

  config = lib.mkIf isGraphicalLinux (
    let
      settings = import ../config/hyprland/settings.nix;
      omarchy = import ./themes/omarchy.nix { inherit lib pkgs; };
      ttfx = pkgs.callPackage ./ttfx.nix { };
      tailscaleStatus = pkgs.writeShellApplication {
        name = "hypr-tailscale-status";
        runtimeInputs = [
          pkgs.tailscale
          pkgs.jq
          pkgs.coreutils
        ];
        text = ''
          raw=$(timeout 5 tailscale status --json 2>/dev/null || true)
          state=$(printf '%s' "$raw" | jq -r '.BackendState // "Unavailable"' 2>/dev/null || printf '%s' Unavailable)
          case "$state" in
            Running)
              class=connected
              label=Connected
              ;;
            NeedsLogin)
              class=warning
              label="Needs login"
              ;;
            *)
              class=disconnected
              label=$state
              ;;
          esac
          jq -nc --arg text "󰖂" --arg class "$class" --arg tooltip "Tailscale: $label" \
            '{text:$text, class:$class, tooltip:$tooltip}'
        '';
      };
      screensaver = pkgs.writeShellApplication {
        name = "hypr-screensaver";
        runtimeInputs = [
          ttfx
          pkgs.hyprland
          pkgs.jq
          pkgs.procps
        ];
        text = lib.removePrefix "#!/usr/bin/env bash\n" (
          builtins.readFile ../config/hyprland/screensaver.sh
        );
      };
      ghosttyScreensaverConfig = pkgs.writeText "ghostty-screensaver" ''
        window-padding-x = 0
        window-padding-y = 0
        window-padding-color = extend-always
        gtk-single-instance = false
        class = org.omarchy.screensaver
        font-size = 18
      '';
      launchScreensaver = pkgs.writeShellApplication {
        name = "hypr-launch-screensaver";
        runtimeInputs = [
          pkgs.ghostty
          pkgs.hyprland
          pkgs.jq
          pkgs.socat
          screensaver
        ];
        text = ''
          export GHOSTTY_SCREENSAVER_CONFIG=${lib.escapeShellArg ghosttyScreensaverConfig}
          ${lib.removePrefix "#!/usr/bin/env bash\n" (
            builtins.readFile ../config/hyprland/launch-screensaver.sh
          )}
        '';
      };
      stopScreensaver = pkgs.writeShellScript "hypr-stop-screensaver" ''
        ${pkgs.hyprland}/bin/hyprctl eval 'hl.config({ cursor = { invisible = false } })' >/dev/null 2>&1 \
          || ${pkgs.hyprland}/bin/hyprctl keyword cursor:invisible false >/dev/null 2>&1 \
          || true
        ${pkgs.procps}/bin/pkill -x ttfx >/dev/null 2>&1 || true
        ${pkgs.procps}/bin/pkill -f '[o]rg.omarchy.screensaver' >/dev/null 2>&1 || true
      '';
      catalogue = import ./themes/palettes.nix;
      themeRender = import ./themes/hyprland.nix { inherit lib; };
      voiceFeatures = import ./voice-supported.nix { inherit pkgs hostName; };
      themeState = "${config.xdg.stateHome}/theme-menu/active";
      themeFile = name: "${themeState}/${name}";
      paletteFor =
        name:
        if name == null then
          hostTheme
        else
          catalogue.${name} or (
            let
              matches = lib.filterAttrs (
                _: theme:
                theme.herdr.name == name || theme.herdr.dark_name == name || theme.herdr.light_name == name
              ) catalogue;
            in
            if matches == { } then
              throw "hyprland appearance.palette is unknown: ${name}"
            else
              builtins.head (lib.attrValues matches)
          );
      selectedPalette = paletteFor settings.appearance.palette;
      wallpaper = pkgs.writeShellScript "hypr-wallpaper" ''
        image=${lib.escapeShellArg (themeFile "wallpaper.png")}
        # theme-menu materialises this file. Do not fall back to a store image:
        # that would keep every selected wallpaper in the Home Manager closure.
        if [ ! -s "$image" ]; then
          exit 0
        fi
        exec ${pkgs.swaybg}/bin/swaybg --image "$image" --mode ${lib.escapeShellArg settings.wallpaper.mode}
      '';
      base16Tokens = [
        "base00"
        "base01"
        "base02"
        "base03"
        "base04"
        "base05"
        "base06"
        "base07"
        "base08"
        "base09"
        "base0A"
        "base0B"
        "base0C"
        "base0D"
        "base0E"
        "base0F"
      ];
      paletteOverrides =
        mode:
        let
          raw = settings.appearance.paletteOverrides.${mode} or { };
        in
        lib.mapAttrs (
          name: value:
          let
            colour = toString value;
          in
          if !(builtins.elem name base16Tokens) then
            throw "appearance.paletteOverrides.${mode}.${name} is not a Base16 token"
          else if builtins.match "[0-9A-Fa-f]{6}" colour == null then
            throw "appearance.paletteOverrides.${mode}.${name} must be six hex digits"
          else
            colour
        ) raw;
      renderMode =
        mode:
        themeRender.render {
          inherit mode;
          inherit (settings) appearance;
          palette = selectedPalette.${mode} // paletteOverrides mode;
        };
      modeAssets =
        mode:
        pkgs.linkFarm "hyprland-theme-${mode}" (
          lib.mapAttrsToList (name: text: {
            inherit name;
            path = pkgs.writeText "hyprland-${mode}-${builtins.replaceStrings [ "/" ] [ "-" ] name}" text;
          }) (renderMode mode)
        );
      themeFallback = pkgs.linkFarm "hyprland-theme-fallback" [
        {
          name = "dark";
          path = modeAssets "dark";
        }
        {
          name = "light";
          path = modeAssets "light";
        }
      ];
      themeSeed = pkgs.writeShellApplication {
        name = "hypr-theme-seed";
        runtimeInputs = [ pkgs.coreutils ];
        text = ''
          state="''${XDG_STATE_HOME:-$HOME/.local/state}/theme-menu/active"
          mkdir -p "$state"
          mode=${selectedPalette.nativeMode}
          if [ -f "$state/../mode" ]; then
            mode="$(< "$state/../mode")"
          fi
          if [ -n "''${HYPR_THEME_MODE:-}" ]; then
            mode="$HYPR_THEME_MODE"
          fi
          case "$mode" in
            light | dark) ;;
            *)
              printf 'hypr-theme-seed: unknown mode %s\n' "$mode" >&2
              exit 1
              ;;
          esac
          for name in hyprland.conf fuzzel.ini walker.css waybar.css hyprlock.conf gtk.css mako.conf colors.toml shell.toml; do
            if [ ! -s "$state/$name" ]; then
              install -m 0644 ${themeFallback}/"$mode/$name" "$state/$name"
            fi
          done
        '';
      };
      fuzzelConfig = pkgs.writeText "hyprland-fuzzel.ini" ''
        include=${themeFile "fuzzel.ini"}
        width=${toString settings.launcher.width}
        lines=${toString settings.launcher.lines}
        anchor=${settings.launcher.anchor}
        layer=${settings.launcher.layer}
        terminal=${settings.launcher.terminal}
        match-mode=${settings.launcher.matchMode}
        minimal-lines=yes
      '';
      launcher = pkgs.writeShellApplication {
        name = "hypr-launcher-fallback";
        runtimeInputs = [
          pkgs.fuzzel
          themeSeed
        ];
        text = ''
          hypr-theme-seed
          exec fuzzel --config ${fuzzelConfig}
        '';
      };
      # Focus an existing window whose class is one of the :-separated names,
      # otherwise launch the command. The separator is a colon because Hyprland
      # runs bind commands through a shell, where a pipe would split the command.
      openApp = pkgs.writeShellApplication {
        name = "hypr-open";
        runtimeInputs = [
          pkgs.hyprland
          pkgs.jq
        ];
        text = ''
          class_alts="$1"
          shift
          if [ -z "$class_alts" ] || [ "$#" -eq 0 ]; then
            echo "usage: hypr-open CLASS[:CLASS...] COMMAND..." >&2
            exit 2
          fi
          address="$(hyprctl clients -j | jq -r --arg alts "$class_alts" '
            ($alts | split(":")) as $want
            | first(.[] | select(
                (.class as $c | $want | index($c))
                or (
                  ($want | index("com.william.cliamp"))
                  and (
                    .title == "cliamp"
                    or (.title | endswith(" | cliamp"))
                  )
                )
              ) | .address) // empty
          ')"
          if [ -n "$address" ]; then
            hyprctl dispatch focuswindow "address:$address"
          else
            exec "$@"
          fi
        '';
      };
      sessionField =
        value:
        if lib.hasInfix "\t" value || lib.hasInfix "\n" value then
          throw "hyprland session menu fields cannot contain tabs or newlines"
        else
          value;
      sessionActions = [
        "lock"
        "display-off"
        "display-toggle"
        "suspend"
        "logout"
        "reboot"
        "poweroff"
      ];
      sessionLabels = map (entry: entry.label) settings.session.entries;
      sessionMenuFile = pkgs.writeText "hypr-session-menu.tsv" (
        lib.concatMapStrings (
          entry:
          if !(builtins.elem entry.action sessionActions) then
            throw "hyprland session action is unknown: ${entry.action}"
          else if lib.count (label: label == entry.label) sessionLabels > 1 then
            throw "hyprland session menu label is duplicated: ${entry.label}"
          else
            lib.concatStringsSep "\t" [
              (sessionField entry.label)
              entry.action
              (if entry.confirm then "1" else "0")
              (sessionField entry.confirmText)
            ]
            + "\n"
        ) settings.session.entries
      );
      record = pkgs.writeShellApplication {
        name = "hypr-record";
        runtimeInputs = [
          pkgs.coreutils
          pkgs.ffmpeg
          pkgs.gpu-screen-recorder
          pkgs.hyprland
          pkgs.jq
          pkgs.libnotify
          pkgs.mpv
          pkgs.nautilus
          pkgs.procps
          pkgs.slurp
          pkgs.wl-clipboard
        ];
        text = builtins.readFile ../config/hyprland/record.sh;
      };
      agentAwake = pkgs.writeShellApplication {
        name = "herdr-agent-awake";
        runtimeInputs = [
          pkgs.coreutils
          pkgs.python3
          pkgs.systemd
        ];
        text = ''
          exec python3 ${../config/hyprland/agent_awake.py}
        '';
      };
      session = pkgs.writeShellApplication {
        name = "hypr-session";
        runtimeInputs = [
          pkgs.coreutils
          pkgs.fuzzel
          pkgs.gawk
          pkgs.hyprlock
          pkgs.systemd
        ];
        text = ''
          export HYPR_SESSION_FUZZEL_CONFIG=${lib.escapeShellArg fuzzelConfig}
          export HYPR_SESSION_PROMPT=${lib.escapeShellArg settings.session.prompt}
          export HYPR_SESSION_CONFIRM_NO=${lib.escapeShellArg settings.session.confirmNo}
          export HYPR_SESSION_CONFIRM_YES=${lib.escapeShellArg settings.session.confirmYes}
          export HYPR_SESSION_MENU_FILE=${sessionMenuFile}
          ${builtins.readFile ../config/hyprland/session.sh}
        '';
      };
      screenshotDirectory =
        if settings.screenshot.directory == null then
          "${config.home.homeDirectory}/Pictures/Screenshots"
        else
          settings.screenshot.directory;
      sattyConfig = (pkgs.formats.toml { }).generate "satty-config.toml" (
        lib.recursiveUpdate {
          general.output-filename = "${screenshotDirectory}/screenshot-%Y%m%d-%H%M%S.png";
        } settings.screenshot.satty
      );
      workspaceBinds = lib.concatMap (n: [
        "${settings.bindings.workspaceMod}, ${toString n}, workspace, ${toString n}"
        "${settings.bindings.workspaceMoveMod}, ${toString n}, movetoworkspace, ${toString n}"
      ]) (lib.genList (i: i + 1) settings.workspaces);
      voiceBinds =
        lib.optionals voiceFeatures.stt [
          settings.voice.interact
          settings.voice.send
          settings.voice.menu
        ]
        ++ lib.optionals voiceFeatures.tts [ settings.voice.read ];
      colourFields = {
        "col.active_border" = "$theme_active_border";
        "col.inactive_border" = "$theme_inactive_border";
      };
      shadowColours = lib.filterAttrs (_: value: value != null) {
        color = settings.appearance.colors.shadow;
        color_inactive = settings.appearance.colors.shadowInactive;
      };
      waybarStyle = ''
        @import url("file://${themeFile "waybar.css"}");

        window#waybar {
          padding: 4px 0;
          background: alpha(@background, ${toString settings.bar.opacity});
        }

        tooltip {
          background: @background;
          color: @text;
        }

        #workspaces button {
          padding: 4px 0;
          color: @muted;
        }

        #workspaces button.active {
          color: @accent;
          background: @selection;
        }

        .module {
          padding: 5px 0;
        }

        #battery.critical:not(.charging), #pulseaudio.muted, #custom-recording,
        #custom-notifications.quiet {
          color: @red;
        }

        /* Optical centring for the asymmetric Wi-Fi glyph. */
        #network.wifi {
          padding-right: 4px;
        }

        #idle_inhibitor.activated {
          color: @accent;
        }
      '';
    in
    assert lib.assertMsg (
      settings.idle.screensaverSeconds < settings.idle.lockSeconds
      && settings.idle.lockSeconds < settings.idle.displayOffSeconds
    ) "idle timeouts must run screensaver, then lock, then display off";
    {
      home.packages = [
        launcher
        openApp
        pkgs.grimblast
        record
        session
        screensaver
        launchScreensaver
        themeSeed
        pkgs.brightnessctl
        pkgs.playerctl
        pkgs.fuzzel
        pkgs.grim
        pkgs.hyprlock
        pkgs.satty
        pkgs.slurp
        pkgs.wl-clipboard
      ];

      home.activation.seedHyprlandTheme = lib.hm.dag.entryBefore [ "applySelectedPalette" ] ''
        run ${themeSeed}/bin/hypr-theme-seed
      '';

      # Legacy hyprlang, not hyprland.lua. 0.55 prefers Lua when both exist,
      # and the theme fragment is a sourced hyprlang file.
      wayland.windowManager.hyprland = {
        enable = true;
        # stateVersion 26.05 defaults to a Lua emitter that is not the 0.55 API.
        # Hyprland uses hyprlang when hyprland.lua is absent.
        configType = "hyprlang";
        package = pkgs.hyprland;
        portalPackage = null;
        settings = {
          source = themeFile "hyprland.conf";
          monitor = ",preferred,auto,auto";
          exec-once = [ "theme-menu --reapply" ];
          "$mainMod" = settings.bindings.mainMod;
          exec-shutdown = "${pkgs.systemd}/bin/systemctl --user stop hyprland-session.target";
          general = {
            gaps_in = settings.appearance.gapsIn;
            gaps_out = settings.appearance.gapsOut;
            float_gaps = settings.appearance.floatGaps;
            layout = settings.window.layout;
            resize_on_border = settings.window.resizeOnBorder;
          }
          // colourFields;
          decoration = {
            active_opacity = settings.appearance.activeOpacity;
            inactive_opacity = settings.appearance.inactiveOpacity;
            shadow = {
              enabled = settings.appearance.shadow;
              range = settings.appearance.shadowRange;
              render_power = 3;
            }
            // shadowColours;
            blur = {
              enabled = settings.appearance.blur;
              size = 3;
              passes = 1;
            };
          };
          dwindle.preserve_split = settings.window.preserveSplit;
          env = [
            "XCURSOR_THEME,${settings.cursor.theme}"
            "XCURSOR_SIZE,${toString settings.cursor.size}"
            "HYPRCURSOR_SIZE,${toString settings.cursor.size}"
            # Qt's built-in file dialog is the light Fusion navigator. gtk3
            # uses the themed GTK chooser. Packaged Qt apps already ship the
            # FileChooser schema; do not set this without that schema.
            "QT_QPA_PLATFORMTHEME,gtk3"
          ];
          cursor = {
            hide_on_key_press = settings.cursor.hideOnKeyPress;
            warp_on_change_workspace = settings.cursor.warpOnChangeWorkspace;
          };
          input = {
            kb_layout = settings.window.keyboardLayout;
            follow_mouse = if settings.window.focusFollowsCursor then 1 else 0;
            touchpad.natural_scroll = settings.window.naturalScroll;
          };
          misc = {
            disable_hyprland_logo = true;
            force_default_wallpaper = 0;
            mouse_move_enables_dpms = true;
            key_press_enables_dpms = true;
            font_family = "$theme_mono_font";
          };
          group = {
            "col.border_active" = "$theme_accent";
            "col.border_inactive" = "$theme_border";
            "col.border_locked_active" = "$theme_check";
            "col.border_locked_inactive" = "$theme_muted";
          };
          bind =
            settings.bindings.bind
            ++ [
              "SUPER, V, exec, ${record}/bin/hypr-record"
              "SUPER ALT, V, exec, ${record}/bin/hypr-record face"
            ]
            ++ workspaceBinds
            ++ voiceBinds;
          inherit (settings.bindings) bindr bindm bindle;
          windowrule =
            map (rule: {
              inherit (rule) name float;
              "match:class" = rule.class;
            }) settings.floating.rules
            ++ [
              {
                name = "daily-agenda";
                "match:class" = "^com\\.mitchellh\\.ghostty$";
                "match:title" = "^Today$";
                float = true;
                center = true;
                size = "760 460";
              }
              {
                name = "daily-notes";
                "match:class" = "^com\\.mitchellh\\.ghostty$";
                "match:title" = "^Today's notes$";
                float = true;
                center = true;
                size = "1000 700";
              }
              {
                name = "screensaver";
                "match:class" = "^org\\.omarchy\\.screensaver$";
                float = true;
                fullscreen = true;
              }
              {
                name = "webcam-overlay";
                "match:class" = "^WebcamOverlay$";
                float = true;
                pin = true;
                no_initial_focus = true;
              }
              {
                name = "brave-web-apps";
                "match:class" =
                  "^brave-(mail[.]google[.]com|drive[.]google[.]com|docs[.]google[.]com|www[.]youtube[.]com|terminus).*";
                float = true;
                center = true;
                size = "1100 800";
              }
            ]
            ++ map (
              rule:
              {
                inherit (rule) name workspace;
              }
              // lib.optionalAttrs (rule ? class) {
                "match:class" = rule.class;
              }
              // lib.optionalAttrs (rule ? title) {
                "match:title" = rule.title;
              }
            ) settings.workspace.rules;
        };
      };

      programs.waybar = {
        enable = true;
        systemd.enable = false;
        settings = [
          (lib.recursiveUpdate {
            layer = "top";
            position = settings.bar.position;
            width = settings.bar.width;
            exclusive = true;
            modules-left = settings.bar.modulesLeft;
            modules-center = settings.bar.modulesCenter;
            modules-right = settings.bar.modulesRight;
            "hyprland/workspaces" = {
              format = "{id}";
              on-click = "activate";
              all-outputs = true;
            };
            "custom/launcher" = {
              format = "󱄅";
              tooltip-format = "Applications · right-click Files";
              on-click = settings.bar.commands.launcher;
              on-click-right = settings.bar.commands.files;
            };
            "custom/recording" = {
              exec = "printf '%s' '●'";
              exec-if = "${record}/bin/hypr-record status";
              signal = 8;
              interval = "once";
              tooltip = "Recording · Super+V to stop";
              on-click = "${record}/bin/hypr-record";
            };
            clock = {
              format = "{:%H\n%M}";
              tooltip-format = "{:%A %d %B %Y}";
              on-click = settings.bar.commands.calendar;
            };
            tray.spacing = settings.bar.traySpacing;
            pulseaudio = {
              format = "{icon}";
              format-muted = "󰝟";
              format-icons.default = [
                ""
                ""
                ""
              ];
              tooltip-format = "Output: {volume}% · {desc}";
              on-click = settings.bar.commands.audio;
              on-click-right = "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle";
              on-scroll-up = "wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+";
              on-scroll-down = "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-";
            };
            "pulseaudio#microphone" = {
              format = "{format_source}";
              format-source = "";
              format-source-muted = "";
              tooltip-format = "Microphone: {volume_source}%";
              on-click = settings.bar.commands.audio;
              on-click-right = "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle";
              on-scroll-up = "wpctl set-volume -l 1 @DEFAULT_AUDIO_SOURCE@ 5%+";
              on-scroll-down = "wpctl set-volume @DEFAULT_AUDIO_SOURCE@ 5%-";
            };
            mpris = {
              format = "{status_icon}";
              format-paused = "{status_icon}";
              status-icons = {
                playing = "";
                paused = "";
                stopped = "";
              };
              tooltip-format = "{player}: {artist} · {title}";
              on-click = "playerctl play-pause";
              on-click-middle = "playerctl next";
              on-click-right = settings.bar.commands.music;
            };
            network = {
              format-wifi = "";
              format-ethernet = "󰈀";
              format-disconnected = "󰤭";
              tooltip-format-wifi = "{essid} · {signalStrength}%\n{ipaddr}/{cidr}";
              tooltip-format-ethernet = "{ifname}\n{ipaddr}/{cidr}";
              tooltip-format-disconnected = "Disconnected · click to connect";
              on-click = settings.bar.commands.network;
            };
            bluetooth = {
              format = "";
              format-connected = "󰂱";
              format-disabled = "󰂲";
              tooltip-format = "Bluetooth: {status}";
              tooltip-format-connected = "{device_enumerate}";
              tooltip-format-enumerate-connected = "{device_alias}";
              on-click = settings.bar.commands.bluetooth;
            };
            "custom/tailscale" = {
              exec = "${tailscaleStatus}/bin/hypr-tailscale-status";
              return-type = "json";
              format = "{}";
              interval = 15;
              tooltip = true;
              on-click = settings.bar.commands.tailscale;
              on-click-right = "${settings.bar.commands.tailscale} toggle";
            };
            backlight = {
              format = "󰃠";
              tooltip-format = "Brightness: {percent}% · scroll to adjust";
              on-scroll-up = "brightnessctl -e4 -n2 set 5%+";
              on-scroll-down = "brightnessctl -e4 -n2 set 5%-";
            };
            idle_inhibitor = {
              format = "{icon}";
              format-icons = {
                activated = "󰅶";
                deactivated = "󰾪";
              };
              tooltip-format-activated = "Stay awake enabled · click to restore idle locking";
              tooltip-format-deactivated = "Click to stay awake";
            };
            battery = {
              format = "{icon}";
              format-charging = "󰂄";
              format-full = "";
              format-icons = [
                ""
                ""
                ""
                ""
                ""
              ];
              states = {
                warning = 25;
                critical = 10;
              };
              tooltip-format = "{capacity}% · {timeTo}";
              on-click = settings.bar.commands.power;
            };
            "custom/notifications" = {
              exec = "hypr-notification-status";
              return-type = "json";
              format = "{}";
              interval = 5;
              tooltip = true;
              on-click = settings.bar.commands.notifications;
              on-click-right = "${settings.bar.commands.notifications} dnd";
            };
            "custom/session" = {
              format = settings.bar.sessionLabel;
              on-click = settings.bar.commands.power;
              tooltip-format = "Lock, suspend, log out and power";
            };
          } settings.bar.widgets)
        ];
        style = waybarStyle;
      };

      # Unqualified Fuzzel launches inherit the same style as explicit menus.
      xdg.configFile."fuzzel/fuzzel.ini".source = fuzzelConfig;
      xdg.configFile."fuzzel/hyprland.ini".source = fuzzelConfig;
      xdg.configFile."satty/config.toml".source = sattyConfig;
      home.activation.screenshotDirectory = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        run install -d -m 0700 ${lib.escapeShellArg screenshotDirectory}
      '';

      # Theme hyprlock.conf is sourced for $theme_* only. Do not also let that
      # file declare background or input-field: Hyprlock adds a widget per block.
      programs.hyprlock = {
        enable = true;
        settings = {
          source = themeFile "hyprlock.conf";
          general = {
            hide_cursor = true;
            ignore_empty_input = true;
          };
          background = {
            monitor = "";
            # Empty path is a solid fill. path=screenshot is a capture, not a theme.
            path = "";
            color = "$theme_background";
          };
          input-field = {
            monitor = "";
            size = "280, 48";
            position = "0, -40";
            halign = "center";
            valign = "center";
            font_family = "$theme_mono_font";
            font_color = "$theme_text";
            inner_color = "$theme_surface";
            outer_color = "$theme_accent";
            fail_color = "$theme_fail";
            check_color = "$theme_check";
            placeholder_text = "Password";
            rounding = "$theme_rounding";
            outline_thickness = "$theme_border_size";
            fade_on_empty = false;
            dots_center = true;
          };
          label = [
            {
              monitor = "";
              text = "$TIME";
              font_family = "$theme_mono_font";
              font_size = "$theme_font_size";
              color = "$theme_text";
              position = "0, 80";
              halign = "center";
              valign = "center";
            }
          ];
        };
      };

      services.hypridle = {
        enable = true;
        systemdTarget = "hyprland-session.target";
        settings = {
          general = {
            # inhibit_sleep=2 waits for the Wayland lock notification when
            # lock_cmd starts hyprlock and before_sleep_cmd locks the session.
            # A foreground hyprlock in before_sleep either blocks suspend or
            # returns before the lock is up.
            inhibit_sleep = 2;
            lock_cmd = "${stopScreensaver}; ${pkgs.procps}/bin/pidof hyprlock || ${pkgs.hyprlock}/bin/hyprlock";
            before_sleep_cmd = "${pkgs.systemd}/bin/loginctl lock-session";
            after_sleep_cmd = "${pkgs.hyprland}/bin/hyprctl dispatch dpms on";
          };
          listener = [
            {
              # No on-resume. Mapping the screensaver resets ext-idle-notify
              # with no seat input, and that resume is the only one hypridle
              # emits until the next idle. The screensaver exits on input.
              timeout = settings.idle.screensaverSeconds;
              on-timeout = "${launchScreensaver}/bin/hypr-launch-screensaver";
            }
            {
              timeout = settings.idle.lockSeconds;
              on-timeout = "${stopScreensaver}; ${pkgs.systemd}/bin/loginctl lock-session";
            }
            {
              timeout = settings.idle.displayOffSeconds;
              on-timeout = "${pkgs.hyprland}/bin/hyprctl dispatch dpms off";
              on-resume = "${pkgs.hyprland}/bin/hyprctl dispatch dpms on";
            }
          ];
        };
      };

      xdg.configFile."omarchy/branding/screensaver.txt".source = ../config/hyprland/screensaver.txt;

      xdg.configFile."mako/config".text = ''
        include=${themeFile "mako.conf"}
        width=${toString settings.notifications.width}
        anchor=${settings.notifications.anchor}
        default-timeout=${toString settings.notifications.timeoutMs}
        layer=overlay
        icons=1
        max-visible=5
      '';

      xdg.dataFile."theme-menu/omarchy-LICENSE".source = omarchy.license;

      systemd.user.services.hypr-wallpaper = {
        Unit = {
          Description = "Theme wallpaper for the Hyprland session";
          PartOf = [ "hyprland-session.target" ];
          After = [ "hyprland-session.target" ];
          ConditionEnvironment = "WAYLAND_DISPLAY";
          # Theme changes restart this on purpose. The default start limit
          # treats that as a crash loop and then leaves the desktop blank.
          StartLimitIntervalSec = 0;
        };
        Service = {
          ExecStart = toString wallpaper;
          Restart = "on-failure";
          RestartSec = 2;
        };
        Install.WantedBy = [ "hyprland-session.target" ];
      };

      systemd.user.services.waybar = {
        Unit = {
          Description = "Waybar for the Hyprland session";
          PartOf = [ "hyprland-session.target" ];
          After = [ "hyprland-session.target" ];
          ConditionEnvironment = "WAYLAND_DISPLAY";
        };
        Service = {
          ExecStartPre = "${themeSeed}/bin/hypr-theme-seed";
          ExecStart = "${pkgs.waybar}/bin/waybar";
          ExecReload = "${pkgs.coreutils}/bin/kill -SIGUSR2 $MAINPID";
          Restart = "on-failure";
          RestartSec = 2;
        };
        Install.WantedBy = [ "hyprland-session.target" ];
      };

      # Mako no longer owns the notification bus. The unit stays defined so
      # activation removes the old start, and it cannot launch beside Omapager.
      systemd.user.services.mako = {
        Unit = {
          Description = "Retired notification daemon";
          ConditionPathExists = "/var/empty/mako-retired";
        };
        Service.ExecStart = "${pkgs.coreutils}/bin/false";
        Install.WantedBy = lib.mkForce [ ];
      };

      # Herdr reports working, blocked, idle and done. Hold logind idle and
      # sleep only while an agent is working. A missing socket fails open.
      systemd.user.services.herdr-agent-awake = {
        Unit = {
          Description = "Hold idle and sleep while a Herdr agent is working";
          PartOf = [ "hyprland-session.target" ];
          After = [ "hyprland-session.target" ];
        };
        Service = {
          ExecStart = "${agentAwake}/bin/herdr-agent-awake";
          Restart = "on-failure";
          RestartSec = 2;
        };
        Install.WantedBy = [ "hyprland-session.target" ];
      };

      # Authentication is the themed prompt inside hyprland-panels. Do not
      # start hyprpolkitagent as well: polkit accepts only one agent.

    }
  );
}
