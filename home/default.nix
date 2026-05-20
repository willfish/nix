{ pkgs, lib, ... }:
{
  imports = [ ./user ];

  home.username = "william";
  home.homeDirectory = if pkgs.stdenv.isDarwin then "/Users/william" else "/home/william";
  home.stateVersion = "25.05";
  home.enableNixpkgsReleaseCheck = false;
  programs.home-manager.enable = true;

  targets = lib.mkIf pkgs.stdenv.isDarwin {
    darwin = {
      copyApps.enable = true;
      linkApps.enable = false;
    };
  };

  news.display = "silent";
  news.json = lib.mkForce { };
  news.entries = lib.mkForce [ ];
}
