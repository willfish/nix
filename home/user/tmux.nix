{ pkgs-unstable, ...}:
{
  programs.tmux = {
    enable = true;
    plugins = with pkgs-unstable.tmuxPlugins; [
      sensible
      catppuccin
      resurrect
      yank
      tmux-thumbs
    ];
    extraConfig = (builtins.readFile ../config/tmux/tmux.conf);
  };

  programs.tmux.tmuxinator.enable = true;
}
