{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  hostName,
  ...
}:
let
  toml = pkgs.formats.toml { };
  profileBin = "${config.home.profileDirectory}/bin";
  elephant =
    (pkgs.elephant.override {
      # Do not even load clipboard capture, shell runner, remote search or indexing.
      enabledProviders = [
        "desktopapplications"
        "windows"
        "calc"
        "menus"
        "providerlist"
      ];
    }).overrideAttrs
      (old: {
        patches = (old.patches or [ ]) ++ [ ../config/launcher/no-app-arguments.patch ];
        postPatch = (old.postPatch or "") + ''
          cp ${../config/launcher/arguments_test.go} internal/providers/desktopapplications/launcher_arguments_test.go
        '';
        postCheck = (old.postCheck or "") + ''
          go test ./internal/providers/desktopapplications
        '';
      });
  projects = pkgs.writeShellApplication {
    name = "launcher-projects";
    runtimeInputs = [
      pkgs.python3
      pkgs.ghostty
      pkgs.neovim
      pkgs.xdg-utils
      pkgs.systemd
    ];
    text = ''exec python3 ${../config/launcher/projects.py} "$@"'';
  };
  launcher = pkgs.writeShellApplication {
    name = "hypr-launcher";
    runtimeInputs = [
      pkgs.coreutils
      pkgs.systemd
      pkgs.walker
      elephant
    ];
    text = ''
      export PATH=${lib.escapeShellArg profileBin}:"$PATH"
      ${builtins.readFile ../config/launcher/launch.sh}
    '';
  };
  action = icon: text: keywords: command: {
    inherit icon text keywords;
    actions."menus:default" = command;
  };
  openUrl = url: "${pkgs.xdg-utils}/bin/xdg-open ${lib.escapeShellArg url}";
  sessionUnit = {
    PartOf = [ "hyprland-session.target" ];
    After = [ "hyprland-session.target" ];
  };
in
{
  config = lib.mkIf isGraphicalLinux {
    home.packages = [
      launcher
      projects
    ];

    services.elephant = {
      enable = true;
      package = elephant;
      # This HM revision writes config.toml; Elephant expects elephant.toml.
      settings = { };
    };
    services.walker = {
      enable = true;
      systemd.enable = true;
      enableElephantIntegration = true;
      theme = {
        name = "hyprland";
        style = ''@import url("file://${config.xdg.stateHome}/theme-menu/active/walker.css");'';
      };
      settings = {
        close_when_open = true;
        hide_action_hints = false;
        hide_quick_activation = true;
        # No query text is ever routed to a shell runner or network provider.
        providers = {
          default = [
            "menus:daily"
            "desktopapplications"
            "windows"
            "menus:projects"
            "menus:desktop"
          ];
          empty = [
            "menus:daily"
            "desktopapplications"
            "menus:desktop"
          ];
          max_results = 40;
          prefixes = [
            {
              prefix = ";";
              provider = "providerlist";
            }
            {
              prefix = "=";
              provider = "calc";
            }
            {
              prefix = "$";
              provider = "windows";
            }
            {
              prefix = "/";
              provider = "menus:projects";
            }
            {
              prefix = ":";
              provider = "menus:desktop";
            }
          ];
          actions = {
            "menus:daily" = [
              {
                action = "menus:default";
                label = "open";
                default = true;
                bind = "Return";
                after = "Close";
              }
            ];
            "menus:desktop" = [
              {
                action = "menus:default";
                label = "open";
                default = true;
                bind = "Return";
                after = "Close";
              }
            ];
            "menus:projects" = [
              {
                action = "terminal";
                label = "terminal";
                default = true;
                bind = "Return";
                after = "Close";
              }
              {
                action = "editor";
                label = "editor";
                bind = "ctrl Return";
                after = "Close";
              }
              {
                action = "files";
                label = "files";
                bind = "alt Return";
                after = "Close";
              }
            ];
          };
        };
        placeholders.default = {
          input = "Apps, windows, projects and actions";
          list = "No matches";
        };
      };
    };

    xdg.configFile = {
      "elephant/elephant.toml".source = toml.generate "elephant.toml" {
        auto_detect_launch_prefix = false;
        # Applications must survive an Elephant restart or a theme change.
        launch_prefix = "${pkgs.systemd}/bin/systemd-run --user --scope --collect --quiet";
        terminal_cmd = "${pkgs.ghostty}/bin/ghostty -e";
      };
      "elephant/desktopapplications.toml".source = toml.generate "desktopapplications.toml" {
        history = true;
        history_when_empty = true;
        only_search_title = false;
        window_integration = true;
        wm_integration = false;
        show_actions = true;
        show_actions_without_query = false;
      };
      "elephant/menus/daily.toml".source = toml.generate "daily-menu.toml" {
        name = "daily";
        name_pretty = "Daily";
        fixed_order = true;
        history = false;
        entries = [
          (action "󰎞" "Today's notes" [ "today" "notes" "daily" ] "${profileBin}/daily-workflow notes")
          (action "󰃭" "Today's agenda" [
            "reminders"
            "calendar"
            "meetings"
            "daily"
          ] "${profileBin}/daily-workflow agenda")
        ];
      };
      "elephant/menus/desktop.toml".source = toml.generate "desktop-menu.toml" {
        name = "desktop";
        name_pretty = "Desktop";
        icon = "preferences-system";
        history = true;
        entries = [
          (action "" "Pi workspace" [
            "pi"
            "mux"
            "herdr"
            "dot"
            "daily"
          ] "${profileBin}/daily-workflow workspace")
          (action "󰃢" "System cleanup (confirm)" [
            "gcall"
            "garbage"
            "generations"
            "nix"
          ] "${profileBin}/daily-workflow cleanup")
          (action "󰸌" "Appearance" [ "theme" "colours" "wallpaper" ] "${profileBin}/theme-menu")
          (action "󰕾" "Audio controls" [ "volume" "microphone" "sound" ] "${profileBin}/hypr-controls audio")
          (action "" "Bluetooth controls" [ "headphones" "pair" ] "${profileBin}/hypr-controls bluetooth")
          (action "󰖩" "Network controls" [ "wifi" "internet" "vpn" ] "${profileBin}/hypr-controls network")
          (action "" "Session menu" [
            "lock"
            "logout"
            "reboot"
            "power"
            "suspend"
          ] "${profileBin}/hypr-session menu")
          (action "󰚩" "Qwen chat on Relay" [ "ai" "llama" "assistant" "remote" ] (
            openUrl "http://relay.taile09696.ts.net:8081"
          ))
        ]
        ++ lib.optionals (import ./voice-supported.nix { inherit pkgs hostName; }).stt [
          (action "󰍬" "Voice picker" [ "dictation" "speech" "model" ] "${profileBin}/voice-menu")
        ]
        ++ lib.optionals (hostName == "andromeda") [
          (action "󰚩" "Qwen chat on Andromeda" [ "ai" "llama" "assistant" "local" ] (
            openUrl "http://127.0.0.1:8081"
          ))
        ];
      };
      "elephant/menus/projects.lua".text = ''
        Name = "projects"
        NamePretty = "Projects"
        Icon = "folder"
        Cache = false
        History = true
        SearchName = true
        Actions = {
          terminal = "${projects}/bin/launcher-projects open %VALUE% terminal",
          editor = "${projects}/bin/launcher-projects open %VALUE% editor",
          files = "${projects}/bin/launcher-projects open %VALUE% files"
        }
        function GetEntries()
          local handle = io.popen("${projects}/bin/launcher-projects list")
          if not handle then return {} end
          local data = handle:read("*a")
          handle:close()
          local ok, entries = pcall(jsonDecode, data)
          if not ok then return {} end
          return entries
        end
      '';
    };

    systemd.user.services = {
      elephant = {
        Unit = sessionUnit;
        Install.WantedBy = lib.mkForce [ "hyprland-session.target" ];
        Service = {
          Environment = [
            "PATH=${profileBin}:${
              lib.makeBinPath [
                pkgs.wl-clipboard
                pkgs.systemd
                pkgs.xdg-utils
              ]
            }"
          ];
          UMask = "0077";
        };
      };
      walker = {
        Unit = sessionUnit;
        Install.WantedBy = lib.mkForce [ "hyprland-session.target" ];
        Service = {
          ExecStartPre = "${profileBin}/hypr-theme-seed";
          # Walker invokes the Elephant CLI as well as using its socket.
          Environment = [ "PATH=${lib.makeBinPath [ elephant ]}:${profileBin}" ];
        };
      };
    };
  };
}
