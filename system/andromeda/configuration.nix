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
    # Leave both NVIDIA suspend integrations off. The systemd path preserves every
    # video-memory allocation and can stall suspend while that copy runs. The kernel
    # notifier path has wedged nvidia-modeset after resume on this card. The default
    # kernel power-management path preserves only select allocations; CUDA and
    # Vulkan clients may need restarting after resume.
    powerManagement.enable = false;
    powerManagement.kernelSuspendNotifier = false;
    nvidiaSettings = true;
    # 595 defaults to kernel suspend notifiers off. PreserveVideoMemoryAllocations=2
    # (auto) follows that setting. Recheck defaults before changing driver branches.
    # 595.99.02 includes NVIDIA's DIFR suspend/resume fix:
    # https://github.com/NVIDIA/open-gpu-kernel-modules/pull/1286#issuecomment-5442437574
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
