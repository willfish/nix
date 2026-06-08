{ config, ... }:
{
  home.sessionVariables = {
    BROWSER = "brave";
    DEFAULT_BROWSER = "brave";
    EDITOR = "nvim";
    GIT_PAGER = "delta";
    TERMINAL = "ghostty";
    # Work around ssh client rejecting Nix store ssh_config snippets
    # (e.g. systemd's 20-systemd-ssh-proxy.conf owned by nobody:0444).
    # Without this, git@github.com operations fail with "Bad owner or
    # permissions". -F /dev/null (or "none") skips all config files
    # (safe here since no ~/.ssh/config is managed). Affects git pull/push
    # etc. via GIT_SSH_COMMAND.
    GIT_SSH_COMMAND = "ssh -F /dev/null";
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
