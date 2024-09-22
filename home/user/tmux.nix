{ pkgs-unstable, ...}:
{
  programs.tmux = {
    enable = true;
    plugins = with pkgs-unstable.tmuxPlugins; [
      catppuccin
      fuzzback
      fzf-tmux-url
      jump
      resurrect
      sensible
      tmux-thumbs
      yank
    ];
    extraConfig = (builtins.readFile ../config/tmux/tmux.conf);
  };

  programs.tmux.tmuxinator.enable = true;
}
