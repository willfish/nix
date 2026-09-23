{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  ...
}:
let
  settings = import ../config/hyprland/settings.nix;
  source = import ./themes/omarchy-source.nix;
  local = ../config/hyprland/controls;
  bundle = pkgs.runCommand "omarchy-panels" { nativeBuildInputs = [ pkgs.patch ]; } ''
    mkdir -p "$out/shell/plugins/panels"
    cp -r ${source}/shell/Ui ${source}/shell/Commons "$out/shell/"
    for panel in audio bluetooth network; do
      cp -r ${source}/shell/plugins/panels/"$panel" "$out/shell/plugins/panels/"
    done
    chmod -R u+w "$out"
    patch --batch -d "$out" -p1 < ${local}/nixos.patch
    cp ${local}/shell.qml "$out/shell/shell.qml"
    cp ${source}/LICENSE "$out/LICENSE"
  '';
  # Only these reviewed helpers enter the panel process's PATH, not Omarchy's
  # installer, updater, session launcher or plugin loader.
  helpers = pkgs.runCommand "omarchy-panel-helpers" { } ''
    mkdir -p "$out/bin"
    for name in audio-output-set-default audio-input-set-default audio-output-sink \
      audio-sink-availability network-status network-band cmd-present bluetooth-power; do
      cp ${source}/bin/omarchy-"$name" "$out/bin/"
    done
    chmod -R u+w "$out"
    substituteInPlace "$out/bin/omarchy-audio-sink-availability" \
      --replace-fail 'fronted="$(omarchy-audio-tuning fronted-sink 2>/dev/null || true)"' 'fronted=""'
    patchShebangs "$out/bin"
  '';
  bluetoothDevice = pkgs.writeShellApplication {
    name = "omarchy-bluetooth-device";
    runtimeInputs = [
      pkgs.bluez
      pkgs.coreutils
      pkgs.blueman
    ];
    text = ''
      action=''${1:-}
      address=''${2:-}
      [[ $address =~ ^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$ ]] || exit 2
      case "$action" in
        connect|disconnect) exec timeout 20 bluetoothctl "$action" "$address" ;;
        forget) exec timeout 20 bluetoothctl remove "$address" ;;
        pair) exec blueman-manager ;;
        *) exit 2 ;;
      esac
    '';
  };
  browser = pkgs.writeShellApplication {
    name = "omarchy-launch-browser";
    runtimeInputs = [ pkgs.xdg-utils ];
    text = ''exec xdg-open "$@"'';
  };
  runtime = with pkgs; [
    bash
    coreutils
    gawk
    gnused
    gnugrep
    networkmanager
    networkmanagerapplet
    iproute2
    iw
    iputils
    jq
    wireplumber
    pulseaudio
    bluez
    util-linux
    fontconfig
    wl-clipboard
    blueman
    libnotify
    helpers
    bluetoothDevice
    browser
    quickshell
  ];
  shell = pkgs.writeShellApplication {
    name = "hypr-controls-shell";
    runtimeInputs = runtime;
    text = ''
      export HYPR_CONTROLS_THEME=${lib.escapeShellArg "${config.xdg.stateHome}/theme-menu/active"}
      export HYPR_CONTROLS_SETTINGS=${
        lib.escapeShellArg (
          builtins.toJSON {
            inherit (settings.bar) position width;
            font = settings.appearance.monoFont;
          }
        )
      }
      export OMARCHY_MENU_FONT=${lib.escapeShellArg settings.appearance.monoFont}
      exec quickshell --no-color --path ${bundle}/shell
    '';
  };
  launcher = pkgs.writeShellApplication {
    name = "hypr-controls";
    runtimeInputs = [
      pkgs.quickshell
      pkgs.systemd
      pkgs.jq
      pkgs.gnugrep
      pkgs.coreutils
      pkgs.libnotify
    ];
    text = ''
      export HYPR_CONTROLS_CONFIG=${bundle}/shell
      ${builtins.readFile (local + "/launch.sh")}
    '';
  };
in
{
  config = lib.mkIf isGraphicalLinux {
    # Keep nm-applet out of the profile's XDG autostart directories. The
    # connection editor remains available to the panel through its wrapper.
    home.packages = [
      launcher
      pkgs.quickshell
      pkgs.blueman
    ];
    # Start its pairing agent on demand via D-Bus, not a second login applet.
    xdg.configFile."autostart/blueman.desktop".text = ''
      [Desktop Entry]
      Type=Application
      Name=Blueman Applet
      Hidden=true
    '';
    # Authentication prompts remain visible even when notifications are muted.
    dconf.settings."org/blueman/general".notification-daemon = false;
    systemd.user.services.hyprland-panels = {
      Unit = {
        Description = "Omarchy control panels for Waybar";
        PartOf = [ "hyprland-session.target" ];
        After = [ "hyprland-session.target" ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
        StartLimitIntervalSec = 30;
        StartLimitBurst = 3;
      };
      Service = {
        ExecStart = "${shell}/bin/hypr-controls-shell";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
  };
}
