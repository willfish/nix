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

  systemd.tmpfiles.rules = [
    "d /srv 0755 root root -"
    "d /srv/media 0755 william users -"
    "d /srv/media/photos 0755 william users -"
    "d /srv/media/videos 0755 william users -"
  ];
}
