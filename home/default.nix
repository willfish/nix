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

  gtk.theme = {
    enable = true;
    name = "Arc-Dark";
    package = pkgs-unstable.arc-theme;
  };

  home.pointerCursor = {
    gtk.enable = true;
    x11.enable = true;
    name = "WhiteSur-cursors";
    package = pkgs-unstable.whitesur-cursors;
    size = 24;
  };
}
