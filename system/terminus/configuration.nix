{
  config,
  lib,
  pkgs,
  immichPkgs,
  ...
}:

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
