{
  config,
  pkgs,
  nixos-hardware,
  ...
}:

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
