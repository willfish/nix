{ pkgs, nixos-hardware, ... }:

{
  system.stateVersion = "25.05";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
    nixos-hardware.nixosModules.framework-amd-ai-300-series
  ];
  networking.hostName = "foundation";

  # MT7925 Bluetooth: hci0 fails with "Failed to send wmt func ctrl (-22)" on
  # linux 7.0.7–7.0.9. Fixed in 7.0.10+; pin 7.0.x on this host only.
  boot.kernelPackages = pkgs.linuxPackages_7_0;

  services.power-profiles-daemon.enable = true;

  environment.systemPackages = with pkgs; [
    android-tools
  ];
}
