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
  # Catalogue commit last marked compatible with Omapager 1.1.1.
  pluginSrc = pkgs.fetchFromGitHub {
    owner = "njpatel";
    repo = "omapager";
    rev = "5cde92ac662418a877fc858bb5859a0933526d64";
    hash = "sha256-xCa9XRVafgCkwPYnhCH8ZPG97uz9eCdoj4cmC1VM1Cc=";
  };
  python = pkgs.python3.withPackages (ps: [ ps.pillow ]);
  githubIcon = pkgs.fetchurl {
    url = "https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png";
    hash = "sha256-bW73vt4EFrbr7iAUvlhSV2286JI1/hbqXETtAb1SITI=";
  };
  prepare = ../config/hyprland/omapager/prepare-shell.py;
  bundle =
    pkgs.runCommand "omapager-shell"
      {
        nativeBuildInputs = [
          pkgs.python3
          pkgs.bash
        ];
      }
      ''
                mkdir -p "$out/shell/omapager" "$out/shell/Commons" "$out/shell/Ui"
                cp -r ${omarchySource}/shell/Commons/. "$out/shell/Commons/"
                cp -r ${omarchySource}/shell/Ui/. "$out/shell/Ui/"
                chmod -R u+w "$out"
                python3 ${prepare} "$out"
                cp ${../config/hyprland/omapager/shell.qml} "$out/shell/shell.qml"
                cp -a ${pluginSrc}/. "$out/shell/omapager/"
                chmod -R u+w "$out/shell/omapager"
                rm -rf \
                  "$out/shell/omapager/.git" \
                  "$out/shell/omapager/.github" \
                  "$out/shell/omapager/docs" \
                  "$out/shell/omapager/tests" \
                  "$out/shell/omapager/security" \
                  "$out/shell/omapager/assets" \
                  "$out/shell/omapager/preview.png"
                substituteInPlace "$out/shell/omapager/bin/omapager-run-helper" \
                  --replace-fail 'PYTHON = Path("/usr/bin/python3")' \
                  'PYTHON = Path("${python}/bin/python3")'
                cp ${../config/hyprland/omapager/icon_paths.py} \
                  "$out/shell/omapager/bin/icon_paths.py"
                substituteInPlace "$out/shell/omapager/bin/omapager-icon" \
                  --replace-fail 'from omapager_files import private_dir, write_bytes, write_json, read_json' \
                  'from omapager_files import private_dir, write_bytes, write_json, read_json
        import icon_paths' \
                  --replace-fail 'os.path.expanduser("~/.local/share/icons"),' \
                  'os.path.expanduser("~/.local/share/icons"),
            os.path.expanduser("~/.nix-profile/share/icons"),
            "/run/current-system/sw/share/icons",' \
                  --replace-fail 'os.path.expanduser("~/.local/share/applications"),' \
                  'os.path.expanduser("~/.local/share/applications"),
            os.path.expanduser("~/.nix-profile/share/applications"),
            "/run/current-system/sw/share/applications",' \
                  --replace-fail 'os.path.realpath(hit).startswith(os.path.realpath(base) + os.sep)' \
                  'icon_paths.allowed_icon(hit, [base])' \
                  --replace-fail 'os.path.realpath(fields["Icon"]).startswith(os.path.realpath(d) + os.sep)' \
                  'icon_paths.allowed_icon(fields["Icon"], [d])'
                {
                  echo 'module Omapager'
                  for qml in "$out/shell/omapager"/*.qml; do
                    base=$(basename "$qml" .qml)
                    echo "$base 1.0 $base.qml"
                  done
                } > "$out/shell/omapager/qmldir"
                patchShebangs "$out/shell/omapager/bin"
      '';
  shell = pkgs.writeShellApplication {
    name = "hypr-omapager-shell";
    runtimeInputs = [
      pkgs.bash
      pkgs.bubblewrap
      pkgs.coreutils
      pkgs.quickshell
      pkgs.wl-clipboard
      python
    ];
    text = ''
      config_dir="''${XDG_CONFIG_HOME:-$HOME/.config}/omapager"
      install -d -m 700 -- "$config_dir"
      export HYPR_CONTROLS_THEME=${lib.escapeShellArg "${config.xdg.stateHome}/theme-menu/active"}
      export HYPR_OMAPAGER_SETTINGS=$config_dir/settings.json
      export HYPR_OMAPAGER_BAR=${
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
    name = "hypr-notifications";
    runtimeInputs = [
      pkgs.coreutils
      pkgs.gnugrep
      pkgs.hyprland
      pkgs.jq
      pkgs.libnotify
      pkgs.quickshell
      pkgs.systemd
    ];
    text = ''
      action=''${1:-panel}
      systemctl --user is-active --quiet hyprland-session.target || {
        echo 'Notifications require an active Hyprland session.' >&2
        exit 1
      }
      systemctl --user start omapager.service
      cursor=$(hyprctl -j cursorpos)
      mapfile -t anchor < <(hyprctl -j monitors | jq -r --argjson p "$cursor" '
        . as $monitors |
        [.[] |
          ((if .transform % 2 == 0 then .width else .height end) / .scale) as $w |
          ((if .transform % 2 == 0 then .height else .width end) / .scale) as $h |
          select($p.x >= .x and $p.x < .x + $w and
                 $p.y >= .y and $p.y < .y + $h)] |
        (.[0] // ($monitors | map(select(.focused))[0]) // $monitors[0]) |
        .name, ($p.x - .x), ($p.y - .y)')
      for _ in {1..40}; do
        if quickshell ipc --path ${bundle}/shell show 2>/dev/null | grep -q notifications; then
          case "$action" in
            dnd)
              exec quickshell ipc --path ${bundle}/shell call notifications toggleDnd
              ;;
            *)
              exec quickshell ipc --path ${bundle}/shell call omapager-host toggle \
                "''${anchor[@]}"
              ;;
          esac
        fi
        sleep 0.1
      done
      notify-send 'Notifications' 'Omapager could not start.'
      exit 1
    '';
  };
  status = pkgs.writeShellApplication {
    name = "hypr-notification-status";
    runtimeInputs = [
      pkgs.coreutils
      pkgs.jq
      pkgs.quickshell
    ];
    text = ''
      line=$(quickshell ipc --path ${bundle}/shell call omapager.panel line 2>/dev/null || true)
      tooltip=$(printf '%s' "$line" | jq -r 'fromjson? // . | .line // empty' 2>/dev/null || true)
      class=idle
      case "$tooltip" in
        *"Do Not Disturb"* | *snoozed* | *Snoozed*) class=quiet ;;
      esac
      if [[ -z $tooltip ]]; then
        tooltip='Notifications'
      fi
      jq -nc --arg tooltip "$tooltip" --arg class "$class" \
        '{text:"󰂚", tooltip:$tooltip, class:$class}'
    '';
  };
  githubWatch = pkgs.writeShellApplication {
    name = "github-notification-watch";
    runtimeInputs = [
      pkgs.gh
      pkgs.libnotify
      python
    ];
    text = ''
      exec ${python}/bin/python3 ${../config/hyprland/omapager/github_watch.py}
    '';
  };
in
{
  config = lib.mkIf isGraphicalLinux {
    home.packages = [
      launcher
      status
      githubWatch
    ];
    # Claim the name before a profile copy of Mako can be bus-activated.
    xdg.dataFile."icons/hicolor/512x512/apps/github.png".source = githubIcon;
    xdg.dataFile."applications/github-notifications.desktop".text = ''
      [Desktop Entry]
      Type=Application
      Name=GitHub
      Icon=github
      NoDisplay=true
    '';
    xdg.dataFile."dbus-1/services/org.freedesktop.Notifications.service".text = ''
      [D-BUS Service]
      Name=org.freedesktop.Notifications
      Exec=${shell}/bin/hypr-omapager-shell
      SystemdService=omapager.service
    '';
    systemd.user.services.omapager = {
      Unit = {
        Description = "Omapager notifications";
        PartOf = [ "hyprland-session.target" ];
        After = [ "hyprland-session.target" ];
        Conflicts = [ "mako.service" ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
        StartLimitIntervalSec = 30;
        StartLimitBurst = 3;
      };
      Service = {
        ExecStart = "${shell}/bin/hypr-omapager-shell";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
    systemd.user.services.github-notifications = {
      Unit = {
        Description = "Desktop alerts for new GitHub notifications";
        PartOf = [ "hyprland-session.target" ];
        After = [
          "hyprland-session.target"
          "omapager.service"
        ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
      };
      Service = {
        ExecStart = "${githubWatch}/bin/github-notification-watch";
        Restart = "on-failure";
        RestartSec = 10;
      };
      Install.WantedBy = [ "hyprland-session.target" ];
    };
  };
}
