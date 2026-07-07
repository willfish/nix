{ pkgs, ... }:

{
  system.stateVersion = "26.05";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
  ];

  networking.hostName = "terminus";

  boot.kernelPackages = pkgs.linuxPackages_latest;
}
