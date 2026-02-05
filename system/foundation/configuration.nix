{ nixos-hardware, ... }:

{
  system.stateVersion = "25.05";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
    nixos-hardware.nixosModules.framework-amd-ai-300-series
  ];
  networking.hostName = "foundation";
  services.power-profiles-daemon.enable = true;
}
