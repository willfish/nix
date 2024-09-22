{ pkgs-unstable, ...}:
{
  programs.tmux = {
    enable = true;
    clock24 = true;
    plugins = with pkgs-unstable.tmuxPlugins; [
      # catppuccin
      fuzzback
      fzf-tmux-url
      jump
      resurrect
      sensible
      tmux-thumbs
      yank
      {
        plugin = dracula;
        extraConfig = ''
          set -g @dracula-show-battery false
          set -g @dracula-show-powerline true
          set -g @dracula-refresh-rate 10
          set -g @dracula-plugins "git attached-clients network-bandwidth"
        '';
      }
    ];
    extraConfig = (builtins.readFile ../config/tmux/tmux.conf);
  };

  programs.tmux.tmuxinator.enable = true;
}
