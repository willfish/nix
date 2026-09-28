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
  # Pinned bar-widget plugin. The sync scripts stay out: they expect Google
  # OAuth, and the rail reads the existing private iCal export instead.
  calendarPlugin = pkgs.fetchFromGitHub {
    owner = "tmn73";
    repo = "omarchy-calendar";
    rev = "15509fca18f14b9e7da5d7bf85cdf3d9da665bae";
    hash = "sha256-XfCeAmSyWltREKuDn9/6OdwyvjdtlQWH+9QLRnEOmlY=";
  };
  calendarSettings = pkgs.writeShellApplication {
    name = "hypr-calendar-settings";
    runtimeInputs = [
      pkgs.python3
      pkgs.coreutils
    ];
    text = ''
      dest=${lib.escapeShellArg "${config.xdg.configHome}/hyprland/calendar-settings.json"}
      python3 - "$dest" "$@" <<'PY'
      import json, os, sys, tempfile
      dest = sys.argv[1]
      if len(sys.argv) != 3 or len(sys.argv[2]) > 8000:
          raise SystemExit(2)
      value = json.loads(sys.argv[2])
      if not isinstance(value, dict):
          raise SystemExit(2)
      parent = os.path.dirname(dest)
      os.makedirs(parent, mode=0o700, exist_ok=True)
      fd, temporary = tempfile.mkstemp(prefix=".calendar-settings-", dir=parent)
      try:
          with os.fdopen(fd, "w") as stream:
              json.dump(value, stream)
              stream.write("\n")
              stream.flush()
              os.fchmod(stream.fileno(), 0o600)
          os.replace(temporary, dest)
      except Exception:
          try:
              os.unlink(temporary)
          except OSError:
              pass
          raise
      PY
    '';
  };
  bundle = pkgs.runCommand "omarchy-panels" { nativeBuildInputs = [ pkgs.patch ]; } ''
    mkdir -p "$out/shell/plugins/panels/calendar"
    cp -r ${source}/shell/Ui ${source}/shell/Commons "$out/shell/"
    for panel in audio bluetooth network tailscale; do
      cp -r ${source}/shell/plugins/panels/"$panel" "$out/shell/plugins/panels/"
    done
    cp ${calendarPlugin}/LICENSE ${calendarPlugin}/Model.js ${calendarPlugin}/manifest.json \
      ${calendarPlugin}/*.qml "$out/shell/plugins/panels/calendar/"
    cp -r ${source}/shell/plugins/polkit "$out/shell/plugins/"
    cp -r ${source}/shell/plugins/menu "$out/shell/plugins/"
    chmod -R u+w "$out"
    printf '%s\n' 'module OmarchyMenu' 'Menu 1.0 Menu.qml' > "$out/shell/plugins/menu/qmldir"
    patch --batch -d "$out" -p1 < ${local}/nixos.patch
    # Select mode must show before the unused menu catalogue loads.
    substituteInPlace "$out/shell/plugins/menu/Menu.qml" \
      --replace-fail 'cursorActive = mode !== "input"' \
      'cursorActive = mode !== "input"; root.rowsLoaded = true'
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
    calendarSettings
    quickshell
  ];
  shell = pkgs.writeShellApplication {
    name = "hypr-controls-shell";
    runtimeInputs = runtime;
    text = ''
      # Keep the setuid pkexec ahead of any store copy a runtime input might add.
      export PATH="/run/wrappers/bin:$PATH"
      export HYPR_CONTROLS_THEME=${lib.escapeShellArg "${config.xdg.stateHome}/theme-menu/active"}
      export HYPR_CALENDAR_SETTINGS=${lib.escapeShellArg "${config.xdg.configHome}/hyprland/calendar-settings.json"}
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
  absent = pkgs.writeShellScriptBin "omarchy-cmd-present" ''
    exit 1
  '';
  menuSelect = pkgs.runCommand "omarchy-menu-select" { } ''
    mkdir -p "$out/bin"
    cp ${source}/bin/omarchy-menu-select "$out/bin/omarchy-menu-select"
    chmod u+w "$out/bin/omarchy-menu-select"
    patchShebangs "$out/bin/omarchy-menu-select"
  '';
  keybindingsScript = pkgs.runCommand "omarchy-menu-keybindings-script" { } ''
    install -Dm755 ${source}/bin/omarchy-menu-keybindings "$out/omarchy-menu-keybindings"
    substituteInPlace "$out/omarchy-menu-keybindings" \
      --replace-fail 'echo "SHIFT ALT,L,Copy URL from Web App,sendshortcut,SHIFT ALT,L,"' 'return 0' \
      --replace-fail 'echo "SHIFT ALT,D,Download Video from Web App,sendshortcut,SHIFT ALT,D,"' 'return 0'
    patchShebangs "$out/omarchy-menu-keybindings"
  '';
  omarchyShell = pkgs.writeShellApplication {
    name = "omarchy-shell";
    runtimeInputs = [
      pkgs.quickshell
      pkgs.systemd
      pkgs.gnugrep
      pkgs.coreutils
    ];
    text = ''
      config=${lib.escapeShellArg "${bundle}/shell"}
      quiet=0
      if [[ ''${1:-} == -q ]]; then
        quiet=1
        shift
      fi
      fail() {
        if (( quiet )); then
          exit 0
        fi
        printf '%s\n' "''$1" >&2
        exit 1
      }
      if (( ''$# < 2 )); then
        fail "Usage: omarchy-shell <target> <method> [args...]"
      fi
      if [[ ''$1 == shell && ( ''$2 == summon || ''$2 == toggle ) && ''$# -eq 3 ]]; then
        set -- "''$1" "''$2" "''$3" "{}"
      fi
      ready() {
        quickshell ipc --path "''$config" show 2>/dev/null | grep -qx 'target shell'
      }
      systemctl --user start hyprland-panels.service
      for _ in {1..30}; do
        ready && break
        sleep 0.1
      done
      if ! ready; then
        systemctl --user restart hyprland-panels.service
        for _ in {1..30}; do
          ready && break
          sleep 0.1
        done
      fi
      ready || fail "omarchy-shell is not running"
      if ! output=$(quickshell ipc --path "''$config" call "''$@"); then
        fail "omarchy-shell is not running"
      fi
      case ''$output in
        "Target not found." | "Function not found." | "Too few arguments provided"* | "Too many arguments provided"* | unknown)
          fail "''$output"
          ;;
      esac
      if (( ! quiet )) && [[ -n ''$output ]]; then
        printf '%s\n' "''$output"
      fi
    '';
  };
  keybindings = pkgs.writeShellApplication {
    name = "omarchy-menu-keybindings";
    runtimeInputs = [
      pkgs.hyprland
      pkgs.gawk
      pkgs.gnugrep
      pkgs.jq
      pkgs.perl
      pkgs.libxkbcommon
      pkgs.coreutils
      absent
      menuSelect
      omarchyShell
    ];
    text = ''
      exec ${keybindingsScript}/omarchy-menu-keybindings "''$@"
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
      keybindings
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
