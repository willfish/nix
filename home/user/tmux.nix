{ pkgs-unstable, ...}:
{
  programs.tmux = {
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
          set -g @dracula-show-battery false
          set -g @dracula-show-powerline true
          set -g @dracula-refresh-rate 10
          set -g @dracula-plugins "git attached-clients"
        '';
      }
    ];
    extraConfig = builtins.readFile ../config/tmux/tmux.conf;
  };

  programs.tmux.tmuxinator.enable = true;
}
