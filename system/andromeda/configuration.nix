{
  pkgs,
  ...
}:

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
}
