{ config, ... }:
{
  home.sessionVariables = {
    BROWSER = "brave";
    DEFAULT_BROWSER = "brave";
    EDITOR = "nvim";
    GIT_PAGER = "delta";
    LESS = "-R";
    MANPAGER = "nvim +Man!";
    NH_HOME_FLAKE = "${config.home.homeDirectory}/.dotfiles";
    NIXPKGS_ALLOW_UNFREE = 1;
    PAGER = "less --raw-control-chars -F -X";
    RUBYOPT = "--enable-yjit";
    VISUAL = "nvim";
    fish_greeting = "";
  };

  home.sessionPath = [
    "$HOME/.bin"
    "$HOME/go/bin"
  ];
}
