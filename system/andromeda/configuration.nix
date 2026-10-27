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
    # Trial full VRAM preservation through NVIDIA's systemd/procfs integration.
    # NixOS sets PreserveVideoMemoryAllocations=1 and installs the sleep units.
    # Keep the notifier path separate; reliable resume remains unverified.
    powerManagement.enable = true;
    powerManagement.kernelSuspendNotifier = false;
    moduleParams.nvidia = {
      # A false NixOS notifier option does not override the driver's default.
      NVreg_UseKernelSuspendNotifiers = 0;
      # Disk-backed ext4, with capacity for all 32 GB of VRAM plus margin.
      NVreg_TemporaryFilePath = "/var/tmp";
    };
    nvidiaSettings = true;
    # Retain the diagnostic baseline with NVIDIA's DIFR sleep lifecycle change:
    # https://github.com/NVIDIA/open-gpu-kernel-modules/pull/1286#issuecomment-5442437574
    package = config.boot.kernelPackages.nvidiaPackages.mkDriver {
      version = "595.99.02";
      sha256_64bit = "sha256-6HR3lYv3YwcFSTJL1a1slI66btIQ5EAFs+/4SUD24ew=";
      openSha256 = "sha256-T36x/jx8yQ8l3LFp1rZIrTfcSwbGy8YSAvXOUSptpb4=";
      settingsSha256 = "sha256-GYCcnxfKPrTCrsmd25sMyzfC5cqJQJx0c31haooyTYM=";
      persistencedSha256 = "sha256-VyKtF/HdHPQrHHK6opSO69M72LmnGZtauuchj9uuje8=";
    };
  };
  # The RX 7600 drives the display; the 5090 stays loaded for CUDA and models.
  # amdgpu first makes it the primary X device for the login screen.
  services.xserver.videoDrivers = [
    "amdgpu"
    "nvidia"
  ];
  # Early KMS shows the LUKS prompt and splash on the AMD outputs.
  hardware.amdgpu.initrd.enable = true;
  # AQ_DRM_DEVICES is colon-separated, so by-path names cannot be used directly.
  services.udev.extraRules = ''
    KERNEL=="card*", KERNELS=="0000:83:00.0", SUBSYSTEM=="drm", SUBSYSTEMS=="pci", SYMLINK+="dri/amd-display"
  '';
  # Hyprland opens only the AMD card and never takes 5090 memory.
  environment.sessionVariables.AQ_DRM_DEVICES = "/dev/dri/amd-display";
  # Local AI catalogue models use NVIDIA CDI devices.
  hardware.nvidia-container-toolkit.enable = true;
}
