{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  ...
}:
let
  settings = import ../config/hyprland/settings.nix;
  omarchySource = import ./themes/omarchy-source.nix;
  # Marketplace listing quickshell.spotify. Pinned to the commit the catalogue
  # last marked compatible. The older verification snapshot is not this player.
  pluginSrc = pkgs.fetchFromGitHub {
    owner = "stappmus";
    repo = "Omarchy-Spotify";
    rev = "e7371f38f79e3bf52bd6e3542ea54859f6dc082b";
    hash = "sha256-vw5WhTQxlyXf0YWiubIWnT2IXaVQ5Hl4ANKB1qQZXXA=";
  };
  backend = pkgs.rustPlatform.buildRustPackage {
    pname = "omarchy-spotify-backend";
    version = "1.0.4";
    src = pluginSrc;
    sourceRoot = "source/backend";
    cargoLock = {
      lockFile = "${pluginSrc}/backend/Cargo.lock";
      outputHashes."librespot-audio-0.8.0" = "sha256-aSddkiUgbr5LeALSePVkK/NYoCwMqNjrr13QV5X/Rm0=";
    };
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [
      pkgs.libpulseaudio
      pkgs.openssl
    ];
    doCheck = false;
  };
  bash = "${pkgs.bash}/bin/bash";
  mkdir = "${pkgs.coreutils}/bin/mkdir";
  # The recognisable mark, without installing the official client.
  icons = pkgs.runCommand "spotify-client-icons" { } ''
    mkdir -p "$out/share/icons"
    cp -a ${pkgs.spotify}/share/icons/hicolor "$out/share/icons/"
  '';
  bundle =
    pkgs.runCommand "omarchy-spotify-shell"
      {
        nativeBuildInputs = [
          pkgs.python3
          pkgs.bash
          pkgs.patch
        ];
      }
      ''
        mkdir -p "$out/shell/spotify" "$out/shell/Commons" "$out/shell/Ui"
        cp -r ${omarchySource}/shell/Commons/. "$out/shell/Commons/"
        cp -r ${omarchySource}/shell/Ui/. "$out/shell/Ui/"
        chmod -R u+w "$out/shell"
        patch --batch -d "$out" -p1 < ${../config/hyprland/spotify/color.patch}
        cp ${../config/hyprland/spotify/shell.qml} "$out/shell/shell.qml"
        cp -a ${pluginSrc}/. "$out/shell/spotify/"
        chmod -R u+w "$out/shell/spotify"
        rm -rf "$out/shell/spotify/docs" "$out/shell/spotify/tests" "$out/shell/spotify/preview.png" "$out/shell/spotify/.git"
        substituteInPlace "$out/shell/spotify/DaemonManager.qml" \
          --replace-fail '/usr/bin/bash' '${bash}'
        substituteInPlace "$out/shell/spotify/Service.qml" \
          --replace-fail '/usr/bin/mkdir' '${mkdir}'
        {
          echo 'module Spotify'
          for qml in "$out/shell/spotify"/*.qml; do
            base=$(basename "$qml" .qml)
            echo "$base 1.0 $base.qml"
          done
        } > "$out/shell/spotify/qmldir"
        patchShebangs "$out/shell/spotify/scripts"
        chmod -R u+w "$out/shell/spotify/scripts"
      '';
  runtime = with pkgs; [
    bash
    coreutils
    diffutils
    findutils
    gawk
    gnugrep
    gnused
    jq
    libsecret
    openssl
    socat
    systemd
    python3
    avahi
    procps
    wl-clipboard
    xdg-utils
    quickshell
  ];
  shell = pkgs.writeShellApplication {
    name = "hypr-spotify-shell";
    runtimeInputs = runtime;
    text = ''
      plugin=${bundle}/shell/spotify
      # The plugin's own readiness check requires this exact layout.
      install_dir="$HOME/.local/lib/omarchy-spotify"
      config_dir="''${XDG_CONFIG_HOME:-$HOME/.config}/omarchy-spotify"
      unit_dir="''${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
      install -d -m 700 -- "$install_dir" "$config_dir" "$unit_dir"
      if [[ ! -f $config_dir/spotifyd.conf ]]; then
        install -m 600 -- "$plugin/config/spotifyd.conf" "$config_dir/spotifyd.conf"
      fi
      ln -sfn ${backend}/bin/omarchy-spotify-backend "$install_dir/omarchy-spotify-backend"
      source_id=$(${bash} "$plugin/scripts/backend-source-id.sh")
      binary_hash=$(sha256sum -- "$install_dir/omarchy-spotify-backend")
      printf '%s\n' "$source_id" > "$install_dir/backend-source.sha256"
      printf '%s\n' "''${binary_hash%% *}" > "$install_dir/backend-binary.sha256"
      chmod 600 "$install_dir/backend-source.sha256" "$install_dir/backend-binary.sha256"
      if [[ ! -f $unit_dir/omarchy-spotify.service ]] \
        || ! cmp -s -- "$plugin/systemd/omarchy-spotify.service" "$unit_dir/omarchy-spotify.service"; then
        install -m 644 -- "$plugin/systemd/omarchy-spotify.service" "$unit_dir/omarchy-spotify.service"
        systemctl --user daemon-reload || true
      fi
      export HYPR_CONTROLS_THEME=${lib.escapeShellArg "${config.xdg.stateHome}/theme-menu/active"}
      export HYPR_SPOTIFY_PLUGIN=$plugin
      export HYPR_SPOTIFY_SETTINGS=$config_dir/player.json
      export OMARCHY_MENU_FONT=${lib.escapeShellArg settings.appearance.monoFont}
      exec quickshell --no-color --path ${bundle}/shell
    '';
  };
  launcher = pkgs.writeShellApplication {
    name = "hypr-spotify";
    runtimeInputs = [
      pkgs.quickshell
      pkgs.systemd
      pkgs.gnugrep
      pkgs.coreutils
      pkgs.libnotify
    ];
    text = ''
      systemctl --user is-active --quiet hyprland-session.target || {
        echo 'Spotify requires an active Hyprland session.' >&2
        exit 1
      }
      systemctl --user start hyprland-spotify.service
      for _ in {1..40}; do
        if quickshell ipc --path ${bundle}/shell show 2>/dev/null | grep -q spotify; then
          exec quickshell ipc --path ${bundle}/shell call spotify toggle
        fi
        sleep 0.1
      done
      notify-send 'Spotify' 'The player could not start.'
      exit 1
    '';
  };
  # Super+O focuses the player from any workspace. A second press must not
  # close it, so this opens with show rather than the launcher toggle.
  focus = pkgs.writeShellApplication {
    name = "hypr-spotify-focus";
    runtimeInputs = [
      pkgs.hyprland
      pkgs.jq
      pkgs.quickshell
      pkgs.systemd
      pkgs.gnugrep
      pkgs.coreutils
      pkgs.libnotify
    ];
    text = ''
      focus_existing() {
        address="$(hyprctl clients -j | jq -r 'first(.[] | select(.title == "Omarchy Spotify") | .address) // empty')"
        if [ -z "$address" ]; then
          return 1
        fi
        hyprctl dispatch focuswindow "address:$address"
      }

      if focus_existing; then
        exit 0
      fi

      systemctl --user is-active --quiet hyprland-session.target || {
        echo 'Spotify requires an active Hyprland session.' >&2
        exit 1
      }
      systemctl --user start hyprland-spotify.service
      opened=0
      for _ in {1..40}; do
        if quickshell ipc --path ${bundle}/shell show 2>/dev/null | grep -q spotify; then
          quickshell ipc --path ${bundle}/shell call spotify show >/dev/null || true
          opened=1
          break
        fi
        sleep 0.1
      done
      if [ "$opened" -eq 1 ]; then
        for _ in {1..40}; do
          if focus_existing; then
            exit 0
          fi
          sleep 0.1
        done
      fi
      notify-send 'Spotify' 'The player could not start.'
      exit 1
    '';
  };
in
{
  config = lib.mkIf isGraphicalLinux {
    home.packages = [
      launcher
      focus
      shell
      icons
    ];
    # User desktop files override the profile. xdg.desktopEntries is not
    # installed by this Home Manager, so write the launcher entry directly.
    xdg.dataFile."applications/spotify.desktop" = {
      force = true;
      text = ''
        [Desktop Entry]
        Type=Application
        Name=Spotify
        GenericName=Music Player
        Comment=Spotify in Quickshell
        Exec=hypr-spotify
        Icon=spotify-client
        Terminal=false
        Categories=AudioVideo;Audio;Player;
      '';
    };
    systemd.user.services.hyprland-spotify = {
      Unit = {
        Description = "Omarchy Spotify player";
        PartOf = [ "hyprland-session.target" ];
        After = [ "hyprland-session.target" ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
        StartLimitIntervalSec = 30;
        StartLimitBurst = 3;
      };
      Service = {
        ExecStart = "${shell}/bin/hypr-spotify-shell";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
  };
}
