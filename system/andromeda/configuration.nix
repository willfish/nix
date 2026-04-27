{ pkgs, config, ... }:

{
  system.stateVersion = "24.11";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
  ];
  nixpkgs.config.allowUnfree = true;
  networking.hostName = "andromeda";
  hardware.system76.enableAll = true;
  programs = {
    steam = {
      enable = true;
      remotePlay.openFirewall = true;
      dedicatedServer.openFirewall = true;
      localNetworkGameTransfers.openFirewall = true;
    };
  };
  boot.kernelPackages = pkgs.linuxPackages_6_18;
  hardware.graphics.enable = true;
  hardware.nvidia = {
    modesetting.enable = true;
    powerManagement.enable = true;
    # NVIDIA 595 open modules default to the kernel suspend notifier path.
    # On this RTX 5090/COSMIC setup that path is hitting GSP heartbeat
    # timeouts after S3 resume and wedging nvidia-modeset.
    powerManagement.kernelSuspendNotifier = false;
    # powerManagement.finegrained = true; Fine-grained power management requires offload to be enabled.
    open = true; # Essential for Blackwell
    nvidiaSettings = true;
    package = config.boot.kernelPackages.nvidiaPackages.beta;
  };
  services.xserver.videoDrivers = [ "nvidia" ];
}
