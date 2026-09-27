{
  config,
  pkgs,
  lib,
  ...
}:
{
  imports = [ ./user ];

  home.username = lib.mkDefault "william";
  home.homeDirectory = lib.mkDefault (
    if pkgs.stdenv.isDarwin then "/Users/william" else "/home/william"
  );
  home.stateVersion = "26.05";
  home.enableNixpkgsReleaseCheck = false;
  programs.home-manager.enable = true;
  nix.package = lib.mkIf (!config.dotfiles.privateEnabled) pkgs.nix;
  nix.settings.experimental-features = lib.mkIf (!config.dotfiles.privateEnabled) [
    "nix-command"
    "flakes"
  ];

  targets = lib.mkIf pkgs.stdenv.isDarwin {
    darwin = {
      copyApps.enable = true;
      copyApps.directory = "Applications";
      linkApps.enable = false;
    };
  };

  news.display = "silent";
  news.json = lib.mkForce { };
  news.entries = lib.mkForce [ ];
}
