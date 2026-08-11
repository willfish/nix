{ ... }:

{
  system.stateVersion = "24.11";
  imports = [
    ../modules/workstation.nix
    ./hardware-configuration.nix
  ];

  networking.hostName = "starfish";
}
