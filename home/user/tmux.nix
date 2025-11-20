{ pkgs-unstable, ... }:
{
  programs.tmux = {
    package = pkgs-unstable.tmux;
    enable = true;
    clock24 = true;
    plugins = with pkgs-unstable.tmuxPlugins; [
      fzf-tmux-url
      jump
      resurrect
      sensible
      yank
      tmux-thumbs
      {
        plugin = dracula;
        extraConfig = ''
          set -g @dracula-terraform-label true
          set -g @dracula-show-battery false
          set -g @dracula-show-powerline true
          set -g @dracula-refresh-rate 10
          set -g @dracula-show-fahrenheit false
          set -g @dracula-fixed-location "Kent"
          set -g @dracula-plugins "git cpu-usage ram-usage weather"
        '';
      }
    ];
    extraConfig = builtins.readFile ../config/tmux/tmux.conf;
    tmuxinator.enable = true;
  };
}
