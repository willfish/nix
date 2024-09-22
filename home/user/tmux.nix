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
          set -g @dracula-show-fahrenheit false
          set -g @dracula-fixed-location "Royal Tunbdige Wells, UK"
          set -g @dracula-plugins "cpu-usage gpu-usage ram-usage weather git synchronize-panes"
        '';
      }
    ];
    extraConfig = (builtins.readFile ../config/tmux/tmux.conf);
  };

  programs.tmux.tmuxinator.enable = true;
}
