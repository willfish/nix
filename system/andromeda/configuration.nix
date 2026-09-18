{
  config,
  pkgs,
  ...
}:

{
  system.stateVersion = "24.11";
  imports = [
    ../modules/workstation.nix
    ./hardware-configuration.nix
  ];
  nixpkgs.config.allowUnfree = true;
  networking.hostName = "andromeda";
  hardware.system76.enableAll = true;
  boot.kernelPackages = pkgs.linuxPackages_6_18;
  hardware.graphics.enable = true;
  hardware.nvidia = {
    open = true; # Required for the RTX 5090 (Blackwell).
    modesetting.enable = true;
    powerManagement.enable = true;
    # Retain the previous systemd suspend path while testing the driver fix.
    powerManagement.kernelSuspendNotifier = false;
    nvidiaSettings = true;
    # 595.99.02 fixes DIFR resource handling across suspend/resume:
    # https://github.com/NVIDIA/open-gpu-kernel-modules/pull/1286#issuecomment-5442437574
    # Keep this pin until the flake's production driver includes that fix.
    package = config.boot.kernelPackages.nvidiaPackages.mkDriver {
      version = "595.99.02";
      sha256_64bit = "sha256-6HR3lYv3YwcFSTJL1a1slI66btIQ5EAFs+/4SUD24ew=";
      openSha256 = "sha256-T36x/jx8yQ8l3LFp1rZIrTfcSwbGy8YSAvXOUSptpb4=";
      settingsSha256 = "sha256-GYCcnxfKPrTCrsmd25sMyzfC5cqJQJx0c31haooyTYM=";
      persistencedSha256 = "sha256-VyKtF/HdHPQrHHK6opSO69M72LmnGZtauuchj9uuje8=";
    };
  };
  services.xserver.videoDrivers = [ "nvidia" ];
}
