{ ... }:

{
  system.stateVersion = "24.11";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
  ];

  networking.hostName = "starfish";
}
