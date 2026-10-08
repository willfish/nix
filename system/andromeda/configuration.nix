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
    # 615 defaults to kernel suspend notifiers. The NixOS false option above
    # emits no parameter, so explicitly retain the existing kernel PM path.
    # PreserveVideoMemoryAllocations=2 (auto) follows this notifier setting.
    moduleParams.nvidia.NVreg_UseKernelSuspendNotifiers = 0;
    # Trial the new feature branch for display-memory and suspend failures.
    # https://www.nvidia.com/en-us/drivers/details/280299/
    package = config.boot.kernelPackages.nvidiaPackages.mkDriver {
      version = "615.78.08";
      sha256_64bit = "sha256-Pj9t3cLudnoIGFMAr3vjyyhuznZpjS3eNSRZl4LQf/4=";
      openSha256 = "sha256-HBINiOjL0ZJLIAJeNIBYHBnwgUXtNwPPtnFpAI1YwF4=";
      settingsSha256 = "sha256-inDRpG02sdDgHmlqgu/DsgK8OFdOt1fZIYyXdhlGC/c=";
      persistencedSha256 = "sha256-RzeR6Ldct6MUxjnXRyThdh5Y3jjMehTVg85MtwuWNX4=";
    };
  };
  services.xserver.videoDrivers = [ "nvidia" ];
}
