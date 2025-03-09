{ lib, pkgs-unstable, ... }:
{
  imports = [ ./user ];

  home.username = "william";
  home.homeDirectory = "/home/william";
  home.stateVersion = "24.05";

  programs.home-manager.enable = true;

  services.network-manager-applet.enable = true;

  news.display = "silent";
  news.json = lib.mkForce { };
  news.entries = lib.mkForce [ ];

  gtk = {
    enable = true;
    iconTheme = {
      name = "Adwaita";
      package = pkgs-unstable.adwaita-icon-theme;
    };
  };
}
