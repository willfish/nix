{
  config,
  lib,
  pkgs,
  immichPkgs,
  ...
}:

let
  # Forced command for the dedicated cache-push key: import store paths and
  # GC-root them, nothing else. Every path argument is strictly validated
  # before use; the key gets no shell, pty, or forwarding.
  cachePushRestrict = pkgs.writeShellScript "cache-push-restrict" ''
    set -eu

    nix_store="${config.nix.package}/bin/nix-store"
    roots=/nix/var/nix/gcroots/cache

    valid_path() {
      printf '%s' "$1" | ${pkgs.gnugrep}/bin/grep -qE '^/nix/store/[0-9a-z]{32}-[A-Za-z0-9+._?=-]+$'
    }

    case "''${SSH_ORIGINAL_COMMAND:-}" in
      missing)
        while IFS= read -r path; do
          valid_path "$path" || exit 64
          [ -e "$path" ] || printf '%s\n' "$path"
        done
        ;;
      import)
        exec "$nix_store" --import
        ;;
      root\ *)
        for path in ''${SSH_ORIGINAL_COMMAND#root }; do
          valid_path "$path" || exit 64
          link="$roots/$(basename "$path")"
          rm -f "$link"
          "$nix_store" --add-root "$link" --realise "$path" >/dev/null
        done
        ;;
      *)
        echo "cache-push-restrict: unsupported command" >&2
        exit 64
        ;;
    esac
  '';
in
{
  system.stateVersion = "26.05";
  imports = [
    ../modules/server.nix
    ./hardware-configuration.nix
    ./storage.nix
  ];

  networking.hostName = "terminus";
  networking.hostId = "bd2a3a9a";

  boot.kernelPackages = pkgs.linuxPackages;
  boot.supportedFilesystems = [ "zfs" ];

  services.pi-agent-bus = {
    enable = true;
    listenAddress = "0.0.0.0";
    port = 7420;
    operatorAccess = "tailnet";
    tokenFile = config.sops.secrets.PI_AGENT_BUS_TOKEN.path;
  };

  # Activation-script installations finish before service activation; only the
  # systemd installation mode provides this unit. Keep the global port closed.
  systemd.services.pi-agent-bus = lib.mkIf config.sops.useSystemdActivation {
    after = [ "sops-install-secrets.service" ];
    requires = [ "sops-install-secrets.service" ];
  };

  # Binary cache of this host's store. The firewall stays closed on the LAN;
  # tailscale0 is a trusted interface, so only tailnet clients can reach it.
  # Priority 30 beats cache.nixos.org (40). Populated by the post-build-hook
  # on every NixOS host and by scripts/cache-push for one-off pushes.
  services.harmonia.cache = {
    enable = true;
    signKeyPaths = [ config.sops.secrets.TERMINUS_CACHE_SIGNING_KEY.path ];
    settings.priority = 30;
  };

  systemd.services.harmonia = lib.mkIf config.sops.useSystemdActivation {
    after = [ "sops-install-secrets.service" ];
    requires = [ "sops-install-secrets.service" ];
  };

  # cache-push runs as william over SSH; let that user manage cache GC roots.
  systemd.tmpfiles.rules = [ "d /nix/var/nix/gcroots/cache 0755 william users - -" ];

  # Dedicated identity for the post-build-hook pushers on every NixOS host.
  # Revoke by removing this line; rotate by replacing the key in nix-config.
  users.users.william.openssh.authorizedKeys.keys = [
    ''command="${cachePushRestrict}",no-pty,no-agent-forwarding,no-port-forwarding,no-X11-forwarding ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIDgacCbWe6IiJBlMnORsFeFwVGSiuUB5B0qyRhVvIRKF cache-push''
  ];

  # cache-push roots pushed closures under gcroots/cache; expire those links
  # ahead of the weekly 7-day store GC so the cache cannot grow unbounded.
  systemd.services.cache-gcroot-prune = {
    description = "Expire binary-cache GC roots";
    serviceConfig.Type = "oneshot";
    script = ''
      if [ -d /nix/var/nix/gcroots/cache ]; then
        ${pkgs.findutils}/bin/find /nix/var/nix/gcroots/cache -mindepth 1 -type l -mtime +14 -delete
      fi
    '';
  };
  systemd.timers.cache-gcroot-prune = {
    wantedBy = [ "timers.target" ];
    timerConfig = {
      OnCalendar = "daily";
      Persistent = true;
    };
  };

  services.immich = {
    enable = true;
    # The release-channel Immich 2 package is insecure. Keep the system and
    # PostgreSQL on the release channel, with the server/ML pair from the
    # existing locked unstable input. See the migration gate in host operations.
    package = immichPkgs.immich;
    host = "0.0.0.0";
    port = 2283;
    openFirewall = true;
    mediaLocation = "/srv/media/immich";
  };

  # Match Immich's upstream launcher: its optional Gunicorn admin socket is unused.
  # The service account deliberately has no writable home for the default socket.
  systemd.services.immich-machine-learning.environment.GUNICORN_CMD_ARGS = "--no-control-socket";

  # Self-hosted audiobook/podcast server (ShelfPlayer + LAN/Tailscale).
  # NixOS 26.05 provides 2.36.0, whose JWT refresh grace window keeps iOS
  # sessions alive through app suspend and network handoff.
  # Library lives on tank/media; point the first web-UI library at /srv/media/audiobooks.
  services.audiobookshelf = {
    enable = true;
    host = "0.0.0.0";
    port = 13378; # official ABS default; LAN + Tailscale clients
    openFirewall = true;
  };

  # Allow the service user to read the shared media tree (mode 0755, group users).
  users.users.audiobookshelf.extraGroups = [ "users" ];

}
