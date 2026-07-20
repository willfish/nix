{
  lib,
  pkgs,
  ...
}:
/*
  NetworkManager secret-agent fix for Cosmic (Linux).

  Symptom (andromeda / FishFamille roam failure):
    NetworkManager[…]: device (wlp68s0): no secrets: No agents were available for this request.
    NetworkManager[…]: device (wlp68s0): Activation: failed for connection 'FishFamille'

  After a 4-way handshake timeout, wpa_supplicant reports WRONG_KEY and NM
  re-requests secrets with REQUEST_NEW. Cosmic's network applet does not
  reliably act as an NM secret agent (and dbus rejects some of its calls),
  so activation fails hard and stays down until a manual reconnect.

  Home Manager's services.network-manager-applet unit Requires=tray.target,
  but Cosmic never activates tray.target, so that service never starts.
  We run nm-applet ourselves as a graphical-session service (secret agent
  still registers without a visible tray icon).

  A small companion watcher re-ups Wi-Fi via `nmcli device connect` after an
  unexpected disconnect so recovery uses stored system secrets (normal
  activation path) rather than waiting for a password dialog.
*/
lib.mkIf pkgs.stdenv.isLinux {
  # Icons used by nm-applet notifications / UI if the panel surfaces them.
  xdg.systemDirs.data = [ "${pkgs.networkmanagerapplet}/share" ];

  systemd.user.services.nm-applet = {
    Unit = {
      Description = "NetworkManager applet (secret agent for Wi-Fi re-auth)";
      Documentation = [
        "man:nm-applet(1)"
        "https://networkmanager.dev/"
      ];
      After = [ "graphical-session.target" ];
      PartOf = [ "graphical-session.target" ];
    };

    Service = {
      Type = "simple";
      # --indicator: StatusNotifierItem path (harmless if Cosmic has no tray).
      ExecStart = "${lib.getExe' pkgs.networkmanagerapplet "nm-applet"} --indicator";
      Restart = "on-failure";
      RestartSec = 2;
    };

    Install = {
      WantedBy = [ "graphical-session.target" ];
    };
  };

  systemd.user.services.wifi-auto-reconnect = {
    Unit = {
      Description = "Re-activate Wi-Fi after unexpected NetworkManager disconnect";
      After = [
        "graphical-session.target"
        "nm-applet.service"
      ];
      PartOf = [ "graphical-session.target" ];
    };

    Service = {
      Type = "simple";
      Restart = "on-failure";
      RestartSec = 3;
      # Pin nmcli via store path (service closure only; not added to profile PATH).
      ExecStart = lib.getExe (
        pkgs.writeShellApplication {
          name = "wifi-auto-reconnect";
          runtimeInputs = [
            pkgs.coreutils
            pkgs.gnugrep
            pkgs.gawk
          ];
          text =
            let
              nmcli = lib.getExe' pkgs.networkmanager "nmcli";
            in
            ''
              set -euo pipefail

              wifi_device() {
                ${nmcli} -t -f DEVICE,TYPE,STATE device status \
                  | awk -F: '$2 == "wifi" { print $1; exit }'
              }

              wifi_is_enabled() {
                ${nmcli} -t -f WIFI general | grep -qx enabled
              }

              wifi_is_connected() {
                local dev
                dev="$(wifi_device)"
                [[ -n "$dev" ]] || return 1
                ${nmcli} -t -f DEVICE,STATE device status \
                  | grep -qx "''${dev}:connected"
              }

              try_reconnect() {
                local dev
                dev="$(wifi_device)"
                if [[ -z "$dev" ]]; then
                  return 0
                fi
                if ! wifi_is_enabled; then
                  return 0
                fi
                if wifi_is_connected; then
                  return 0
                fi
                # Normal activation uses system-connection secrets (no REQUEST_NEW).
                ${nmcli} -w 25 device connect "$dev" >/dev/null 2>&1 || true
              }

              # Catch up if we start while already disconnected.
              sleep 2
              try_reconnect

              ${nmcli} monitor | while IFS= read -r line; do
                case "$line" in
                  *disconnected*|*Disconnect*|*failed*|*Failed*|*unavailable*)
                    # Let wpa_supplicant finish ignore-list / scan settle.
                    sleep 5
                    try_reconnect
                    ;;
                esac
              done
            '';
        }
      );
    };

    Install = {
      WantedBy = [ "graphical-session.target" ];
    };
  };
}
