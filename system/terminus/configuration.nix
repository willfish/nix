{ pkgs, ... }:

{
  system.stateVersion = "26.05";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
  ];

  networking.hostName = "terminus";
  networking.hostId = "bd2a3a9a";

  boot.kernelPackages = pkgs.linuxPackages;
  boot.supportedFilesystems = [ "zfs" ];

  services.zfs.autoScrub = {
    enable = true;
    pools = [ "tank" ];
  };

  fileSystems."/srv/media" = {
    device = "tank/media";
    fsType = "zfs";
  };

  services.immich = {
    enable = true;
    host = "0.0.0.0";
    port = 2283;
    openFirewall = true;
    mediaLocation = "/srv/media/immich";
  };

  systemd.tmpfiles.rules = [
    "d /srv 0755 root root -"
    "d /srv/media 0755 william users -"
    "d /srv/media/immich 0700 immich immich -"
    "d /srv/media/imports 0755 william users -"
    "d /srv/media/photos 0755 william users -"
    "d /srv/media/videos 0755 william users -"
    "d /srv/media/phone-backups 0755 william users -"
  ];
}
