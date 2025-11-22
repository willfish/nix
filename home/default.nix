{ lib, ... }:
{
  imports = [ ./user ];

  home.username = "william";
  home.homeDirectory = "/home/william";
  home.stateVersion = "25.05";
  home.enableNixpkgsReleaseCheck = false;
  programs.home-manager.enable = true;

  news.display = "silent";
  news.json = lib.mkForce { };
  news.entries = lib.mkForce [ ];
}
