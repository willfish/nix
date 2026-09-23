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
  config = lib.mkIf isGraphicalLinux (
    let
      settings = import ../config/hyprland/settings.nix;
      omarchy = import ./themes/omarchy.nix { inherit lib pkgs; };
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
      fallbackWallpaper = omarchy.wallpapers.${selectedPalette.herdr.name};
      wallpaper = pkgs.writeShellScript "hypr-wallpaper" ''
        image=${lib.escapeShellArg (themeFile "wallpaper.png")}
        if [ ! -s "$image" ]; then
          image=${lib.escapeShellArg (toString fallbackWallpaper)}
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
        runtimeInputs = [
          pkgs.coreutils
          pkgs.gnugrep
        ];
        text = ''
          state="''${XDG_STATE_HOME:-$HOME/.local/state}/theme-menu/active"
          mkdir -p "$state"
          mode=dark
          cosmic_mode="$HOME/.config/cosmic/com.system76.CosmicTheme.Mode/v1/is_dark"
          if [ -n "''${HYPR_THEME_MODE:-}" ]; then
            mode="$HYPR_THEME_MODE"
          elif [ -f "$cosmic_mode" ] && grep -qx false "$cosmic_mode"; then
            mode=light
          fi
          case "$mode" in
            light | dark) ;;
            *)
              printf 'hypr-theme-seed: unknown mode %s\n' "$mode" >&2
              exit 1
              ;;
          esac
          for name in hyprland.conf fuzzel.ini waybar.css hyprlock.conf gtk.css mako.conf; do
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
        name = "hypr-launcher";
        runtimeInputs = [
          pkgs.fuzzel
          themeSeed
        ];
        text = ''
          hypr-theme-seed
          exec fuzzel --config ${fuzzelConfig}
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
        }

        #workspaces button {
          padding: 4px 0;
          color: @muted;
        }

        #workspaces button.active {
          color: @accent;
          background: @selection;
        }

        #clock,
        #tray,
        #pulseaudio,
        #network,
        #bluetooth,
        #battery,
        #custom-session {
          padding: 6px 0;
        }
      '';
    in
    {
      home.packages = [
        launcher
        pkgs.grimblast
        session
        themeSeed
        pkgs.brightnessctl
        pkgs.cosmic-settings
        pkgs.playerctl
        pkgs.fuzzel
        pkgs.grim
        pkgs.hyprlock
        pkgs.mako
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
          bind = settings.bindings.bind ++ workspaceBinds ++ voiceBinds;
          inherit (settings.bindings) bindr bindm bindle;
          windowrule = map (rule: {
            inherit (rule) name float;
            "match:class" = rule.class;
          }) settings.floating.rules;
        };
      };

      programs.waybar = {
        enable = true;
        systemd.enable = false;
        settings = [
          {
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
            clock = {
              format = "{:%H\n%M}";
              tooltip-format = "{:%A %d %B}";
            };
            tray.spacing = 4;
            pulseaudio = {
              format = "{icon}";
              format-muted = "M";
              on-click = settings.bar.commands.audio;
            };
            network = {
              format-wifi = "W";
              format-ethernet = "E";
              format-disconnected = "X";
              on-click = settings.bar.commands.network;
            };
            bluetooth = {
              format = "B";
              on-click = settings.bar.commands.bluetooth;
            };
            battery = {
              format = "{capacity}";
              on-click = settings.bar.commands.power;
            };
            "custom/session" = {
              format = settings.bar.sessionLabel;
              on-click = "hypr-session menu";
              tooltip = false;
            };
          }
        ];
        style = waybarStyle;
      };

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
            lock_cmd = "${pkgs.procps}/bin/pidof hyprlock || ${pkgs.hyprlock}/bin/hyprlock";
            before_sleep_cmd = "${pkgs.systemd}/bin/loginctl lock-session";
            after_sleep_cmd = "${pkgs.hyprland}/bin/hyprctl dispatch dpms on";
          };
          listener = [
            {
              timeout = settings.idle.lockSeconds;
              on-timeout = "${pkgs.systemd}/bin/loginctl lock-session";
            }
            {
              timeout = settings.idle.displayOffSeconds;
              on-timeout = "${pkgs.hyprland}/bin/hyprctl dispatch dpms off";
              on-resume = "${pkgs.hyprland}/bin/hyprctl dispatch dpms on";
            }
          ];
        };
      };

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

      systemd.user.services.mako = {
        Unit = {
          Description = "Notifications for the Hyprland session";
          PartOf = [ "hyprland-session.target" ];
          After = [ "hyprland-session.target" ];
          ConditionEnvironment = "WAYLAND_DISPLAY";
        };
        Service = {
          ExecStartPre = "${themeSeed}/bin/hypr-theme-seed";
          ExecStart = "${pkgs.mako}/bin/mako";
          ExecReload = "${pkgs.mako}/bin/makoctl reload";
          Restart = "on-failure";
          RestartSec = 2;
        };
        Install.WantedBy = [ "hyprland-session.target" ];
      };

      # COSMIC has its own agent. Do not use services.hyprpolkitagent: that
      # unit follows graphical-session.target and would start in both sessions.
      systemd.user.services.hyprpolkitagent = {
        Unit = {
          Description = "Hyprland PolicyKit authentication agent";
          PartOf = [ "hyprland-session.target" ];
          After = [ "hyprland-session.target" ];
          ConditionEnvironment = "WAYLAND_DISPLAY";
        };
        Service = {
          ExecStart = "${pkgs.hyprpolkitagent}/libexec/hyprpolkitagent";
          Restart = "on-failure";
          RestartSec = 2;
        };
        Install.WantedBy = [ "hyprland-session.target" ];
      };

    }
  );
}
