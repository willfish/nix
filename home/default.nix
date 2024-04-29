{ ... }:

{
  imports = [./user];

  home.username = "william";
  home.homeDirectory = "/home/william";
  home.stateVersion = "23.11";

  programs.home-manager.enable = true;

  services.network-manager-applet.enable = true;
}
