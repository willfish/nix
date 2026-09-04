{
  config,
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

  boot.extraModulePackages = [
    (pkgs.callPackage ./mt7925-patched-driver.nix {
      inherit (config.boot.kernelPackages) kernel;
    })
  ];

  # Keep power-management mitigations alongside the patched MT7925 module set.
  # nixos-hardware's generic MT7925 module also sets mt7925e power_save=0, but
  # this kernel only exposes disable_aspm and disable_clc as module parameters.
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
