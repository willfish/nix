{
  pkgs,
  nixos-hardware,
  ...
}:

{
  system.stateVersion = "25.05";
  imports = [
    ../modules/workstation.nix
    ./hardware-configuration.nix
    nixos-hardware.nixosModules.framework-amd-ai-300-series
  ];
  networking.hostName = "foundation";

  # MT7925 Bluetooth: hci0 failed with "Failed to send wmt func ctrl (-22)" on
  # linux 7.0.7–7.0.9. Use the latest kernel so this host retains the fix.
  boot.kernelPackages = pkgs.linuxPackages_latest;

  # In-tree mt7925e has no power_save parameter. ASPM and CLC still drop the link.
  boot.extraModprobeConfig = ''
    options mt7925e disable_aspm=1
    options mt7925-common disable_clc=1
  '';

  networking.networkmanager.wifi = {
    powersave = false;
    scanRandMacAddress = false;
  };

  services.power-profiles-daemon.enable = true;

  environment.systemPackages = with pkgs; [
    android-tools
  ];
}
