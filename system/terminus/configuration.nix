{ pkgs, ... }:

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

  services.immich = {
    enable = true;
    host = "0.0.0.0";
    port = 2283;
    openFirewall = true;
    mediaLocation = "/srv/media/immich";
  };

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
