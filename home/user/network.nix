{
  config,
  lib,
  pkgs,
  isGraphicalLinux,
  ...
}:
/*
  NetworkManager secret agent for automatic Wi-Fi recovery (Linux).

  Symptom (andromeda / FishFamille roam failure):
    NetworkManager[…]: device (wlp68s0): no secrets: No agents were available for this request.
    NetworkManager[…]: device (wlp68s0): Activation: failed for connection 'FishFamille'
    nm-applet[…]: No keyring secrets found for FishFamille/802-11-wireless-security; asking user.

  After a 4-way handshake timeout, wpa_supplicant reports WRONG_KEY and NM
  re-requests secrets with REQUEST_NEW. Recovery needs a secret agent even
  when no interactive network applet is running.

  nm-applet *does* register as a secret agent, but for system connections
  (psk-flags=0) it only looks in the user keyring, finds nothing, and opens
  an interactive password dialog. While that dialog sits unanswered the
  activation dies and connectivity drops.

  Fix: headless secret agent that re-supplies the PSK already stored in the
  system connection (nmcli -s). REQUEST_NEW is treated like a normal request
  - the password was never actually wrong. No nm-applet / extra tray UI.
  Interactive setup uses the Hyprland network panel, nm-connection-editor,
  nmtui, or nmcli --ask.

  Companion watcher re-ups Wi-Fi after unexpected disconnect, using an
  explicit passwd-file from the system secret so recovery does not depend
  on the agent race.
*/
lib.mkIf (isGraphicalLinux && config.dotfiles.privateEnabled) (
  let
    nmAutoSecretAgent = import ./nm-auto-secret-agent-package.nix { inherit pkgs; };
  in
  {
    systemd.user.services.nm-auto-secret-agent = {
      Unit = {
        Description = "NetworkManager secret agent (auto-submit system Wi-Fi PSKs)";
        Documentation = [
          "man:NetworkManager(8)"
          "https://networkmanager.dev/"
        ];
        After = [ "graphical-session.target" ];
        PartOf = [ "graphical-session.target" ];
      };

      Service = {
        Type = "simple";
        ExecStart = lib.getExe nmAutoSecretAgent;
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
          "nm-auto-secret-agent.service"
        ];
        PartOf = [ "graphical-session.target" ];
      };

      Service = {
        Type = "simple";
        Restart = "on-failure";
        RestartSec = 3;
        # Use the NixOS NetworkManager CLI, not Home Manager's package. HM
        # nixpkgs has been newer than the running daemon (nmcli 1.56 vs NM
        # 1.54), which spams "versions don't match" on every probe.
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
                nmcli = "/run/current-system/sw/bin/nmcli";
              in
              ''
                set -euo pipefail

                # Rate-limit reconnect attempts so a truly wrong PSK cannot spin.
                last_attempt_epoch=0
                min_interval_sec=15

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

                preferred_wifi_connection() {
                  local dev="$1"
                  local conn
                  conn="$(${nmcli} -t -g GENERAL.CONNECTION device show "$dev" 2>/dev/null || true)"
                  if [[ -n "$conn" && "$conn" != "--" ]]; then
                    printf '%s\n' "$conn"
                    return 0
                  fi
                  # Most recently activated Wi-Fi profile.
                  ${nmcli} -t -f NAME,TYPE,TIMESTAMP connection show \
                    | awk -F: '$2 == "802-11-wireless" { print $3 "\t" $1 }' \
                    | sort -nr \
                    | head -n1 \
                    | cut -f2-
                }

                try_reconnect() {
                  local dev conn psk passwd_file now
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

                  now="$(date +%s)"
                  if (( now - last_attempt_epoch < min_interval_sec )); then
                    return 0
                  fi
                  last_attempt_epoch="$now"

                  conn="$(preferred_wifi_connection "$dev" || true)"
                  if [[ -n "$conn" ]]; then
                    psk="$(${nmcli} -s -g 802-11-wireless-security.psk connection show "$conn" 2>/dev/null || true)"
                    if [[ -n "$psk" ]]; then
                      passwd_file="$(mktemp)"
                      trap 'rm -f -- "$passwd_file"' EXIT
                      trap 'exit 130' INT
                      trap 'exit 143' TERM
                      chmod 600 "$passwd_file"
                      # passwd-file format: setting.property:value
                      printf '802-11-wireless-security.psk:%s\n' "$psk" >"$passwd_file"
                      # Explicit secrets avoid the REQUEST_NEW / agent path entirely.
                      ${nmcli} -w 25 connection up "$conn" ifname "$dev" passwd-file "$passwd_file" \
                        >/dev/null 2>&1 || true
                      rm -f "$passwd_file"
                      trap - EXIT INT TERM
                      return 0
                    fi
                  fi

                  # Fallback: normal activation (agent auto-submits system secrets).
                  ${nmcli} -w 25 device connect "$dev" >/dev/null 2>&1 || true
                }

                # Catch up if we start while already disconnected.
                sleep 2
                try_reconnect

                ${nmcli} monitor | while IFS= read -r line; do
                  # p2p-dev-* flaps "disconnected"/"unavailable" constantly and
                  # is not the STA interface we can repair with connection up.
                  case "$line" in
                    p2p-*|*p2p-dev*)
                      continue
                      ;;
                    *": disconnected"*|*": failed"*)
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
)
