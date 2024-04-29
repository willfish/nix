{ config, pkgs, pkgs-unstable, home-manager, ... }:

{
  imports = [./user];

  home.username = "william";
  home.homeDirectory = "/home/william";
  home.stateVersion = "23.11";

  home.sessionVariables = {
    VISUAL = "nvim";
    EDITOR = "nvim";
    ASDF_RUBY_BUILD_VERSION = "master";
    LESS = "-R";
    GIT_PAGER = "delta";
    MANPAGER = "nvim +Man!";
    PAGER = "less --raw-control-chars -F -X";
    RUBYOPT = "--enable-yjit";
    fish_greeting = "";
    XDG_CURRENT_DESKTOP = "gnome";
  };

  programs.home-manager.enable = true;

  services.network-manager-applet.enable = true;
}
