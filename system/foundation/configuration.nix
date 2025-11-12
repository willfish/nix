{
  pkgs-unstable,
  config,
  nixos-hardware,
  ...
}:

{
  system.stateVersion = "25.05";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
    # nixos-hardware.nixosModules.framework-13inch-amd-ai-300-series
  ];
  networking.hostName = "foundation";
}
