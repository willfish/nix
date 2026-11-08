{
  config,
  lib,
  pkgs,
  ...
}:
let
  hub = config.dotfiles.capabilities.ovisHub;
  bridge = config.dotfiles.capabilities.ovisBridge;
  enabled = pkgs.stdenv.isLinux && (hub || bridge);
  state = "${config.xdg.stateHome}/ovis";
  start = pkgs.writeShellApplication {
    name = "ovis-start";
    runtimeInputs = [
      pkgs.coreutils
      pkgs.git
      pkgs.jq
      pkgs.openssh
      pkgs.procps
      pkgs.tailscale
      pkgs.util-linux
    ];
    text = ''
      umask 077
      export PATH="$HOME/.local/bin:$HOME/.nix-profile/bin:/run/current-system/sw/bin:$PATH"
      export HOST=127.0.0.1 PORT=7317
      export OVIS_STATE_DIR=${lib.escapeShellArg state}
      export OVIS_TELEMETRY=0 OVIS_AUTO_UPDATE=0
      # Nix owns releases and ingress. Do not start a public relay or a second listener.
      mkdir -p "$OVIS_STATE_DIR/relay"
      chmod 700 "$OVIS_STATE_DIR" "$OVIS_STATE_DIR/relay"
      printf '{"enabled":false}\n' > "$OVIS_STATE_DIR/relay/state.json"
      printf '{"on":false}\n' > "$OVIS_STATE_DIR/relay/direct.json"
      settings="$OVIS_STATE_DIR/machine-settings.json"
      updated="$(mktemp "$OVIS_STATE_DIR/.machine-settings.XXXXXX")"
      trap 'rm -f "$updated"' EXIT
      if [ -e "$settings" ]; then
        jq '.auto_update_bridges = false' "$settings" > "$updated"
      else
        printf '{"auto_update_bridges":false}\n' > "$updated"
      fi
      mv "$updated" "$settings"
      ${
        if hub then
          ''
            export OVIS_RUNTIME=herdr OVIS_SSH_RUNTIME=herdr-core
            export OVIS_APP_NAME="Ovis (Terminus)"
            OVIS_TAILSCALE_OWNER="$(tailscale status --json | jq -er '.User[.Self.UserID | tostring].LoginName | select(length > 0)')"
            export OVIS_TAILSCALE_OWNER
          ''
        else
          ''
            export OVIS_RUNTIME=herdr-core
            export OVIS_HC_BIN="$HOME/.local/bin/herdr-core"
            # Never start a new core in place of the desktop app's existing session.
            test -S "''${XDG_RUNTIME_DIR:?}/herdr-core/default.sock" || exit 75
          ''
      }
      exec ${pkgs.ovis}/bin/ovis
    '';
  };
in
lib.mkIf enabled {
  home.packages = [ pkgs.ovis ];
  systemd.user.services.ovis = {
    Unit = {
      Description =
        if hub then "Private Ovis multi-host hub" else "Ovis bridge for existing Herdr app sessions";
      After = [ "network-online.target" ];
      StartLimitIntervalSec = 0;
    };
    Service = {
      ExecStart = lib.getExe start;
      Restart = "on-failure";
      RestartSec = 10;
      UMask = "0077";
      NoNewPrivileges = true;
    };
    Install.WantedBy = [ "default.target" ];
  };
}
