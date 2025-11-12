{ pkgs-unstable, config, ... }:

{
  system.stateVersion = "24.11";
  imports = [
    ../modules/common-configuration.nix
    ./hardware-configuration.nix
  ];
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
  boot.kernelPackages = pkgs-unstable.linuxPackages_latest;
  hardware.graphics.enable = true;
  hardware.nvidia = {
    modesetting.enable = true;
    powerManagement.enable = true;
    # powerManagement.finegrained = true; Fine-grained power management requires offload to be enabled.
    open = true; # Essential for Blackwell
    nvidiaSettings = true;
    package = config.boot.kernelPackages.nvidiaPackages.beta;
  };

  services.xserver.videoDrivers = [ "nvidia" ];
}
