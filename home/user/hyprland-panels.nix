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
    for panel in audio bluetooth network tailscale; do
      cp -r ${source}/shell/plugins/panels/"$panel" "$out/shell/plugins/panels/"
    done
    cp -r ${source}/shell/plugins/polkit "$out/shell/plugins/"
    chmod -R u+w "$out"
    patch --batch -d "$out" -p1 < ${local}/nixos.patch
    # pkexec realpath()s its program. The packaged tailscale path is a symlink to
    # tailscaled, so the command starts the daemon and exits before setting an operator.
    substituteInPlace "$out/shell/plugins/panels/tailscale/Service.qml" \
      --replace-fail '["pkexec", "tailscale", "set", "--operator=" + userName]' \
      '["/run/wrappers/bin/pkexec", "${tailscaleCli}", "set", "--operator=" + userName]'
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
  pythonWithGio = pkgs.python3.withPackages (ps: [ ps.pygobject3 ]);
  fileSelect = pkgs.writeShellApplication {
    name = "omarchy-file-select";
    runtimeInputs = [
      pythonWithGio
      pkgs.glib
    ];
    text = ''
      export GI_TYPELIB_PATH="${pkgs.glib}/lib/girepository-1.0''${GI_TYPELIB_PATH:+:$GI_TYPELIB_PATH}"
      exec ${pythonWithGio}/bin/python3 ${source}/bin/omarchy-file-select "$@"
    '';
  };
  notify = pkgs.writeShellApplication {
    name = "omarchy-notification-send";
    runtimeInputs = [ pkgs.libnotify ];
    text = ''
      urgency=low
      while [[ $# -gt 0 ]]; do
        case "$1" in
          -u | --urgency)
            urgency=$2
            shift 2
            ;;
          -g | --glyph)
            shift 2
            ;;
          *) break ;;
        esac
      done
      headline=
      description=
      [[ $# -ge 1 ]] || exit 2
      headline=$1
      shift
      if [[ $# -gt 0 && $1 != -* ]]; then
        description=$1
      fi
      exec notify-send -a Tailscale -u "$urgency" -- "$headline" "$description"
    '';
  };
  # A real file, not the packaged symlink. Forces CLI argv0 after pkexec's shebang reset.
  tailscaleCli = pkgs.writeShellScript "tailscale-cli" ''
    exec -a tailscale ${pkgs.tailscale}/bin/.tailscaled-wrapped "$@"
  '';
  tailscaleSend = pkgs.writeShellApplication {
    name = "omarchy-tailscale-send";
    runtimeInputs = [
      pkgs.tailscale
      pkgs.coreutils
      fileSelect
      notify
    ];
    text = lib.removePrefix "#!/bin/bash\n" (builtins.readFile "${source}/bin/omarchy-tailscale-send");
  };
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
  laptopClosed = pkgs.writeShellApplication {
    name = "omarchy-hw-laptop-closed";
    runtimeInputs = [ pkgs.gnugrep ];
    text = ''
      for state in /proc/acpi/button/lid/*/state; do
        [ -r "$state" ] || continue
        grep -q closed "$state" && exit 0
      done
      exit 1
    '';
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
    which
    tailscale
    helpers
    bluetoothDevice
    browser
    laptopClosed
    tailscaleSend
    quickshell
  ];
  shell = pkgs.writeShellApplication {
    name = "hypr-controls-shell";
    runtimeInputs = runtime;
    text = ''
      # Keep the setuid pkexec ahead of any store copy a runtime input might add.
      export PATH="/run/wrappers/bin:$PATH"
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
        Description = "Themed control panels and authentication prompt";
        PartOf = [ "hyprland-session.target" ];
        After = [ "hyprland-session.target" ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
        StartLimitIntervalSec = 30;
        StartLimitBurst = 3;
      };
      Service = {
        # The shell prompt must be the only authentication agent.
        ExecStartPre = [
          "-${pkgs.systemd}/bin/systemctl --user stop hyprpolkitagent.service"
        ];
        ExecStart = "${shell}/bin/hypr-controls-shell";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
  };
}
