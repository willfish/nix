{ ... }:
let
  configDir = ../config;
in
{
  home.file = {
    ".aprc".source = "${configDir}/aprc";
    ".bin/".source = "${configDir}/bin";
    ".config/btop".source = "${configDir}/btop";
    ".config/ghostty".source = "${configDir}/ghostty";
    ".config/kitty".source = "${configDir}/kitty";
    ".config/nvim/init.lua".source = "${configDir}/kickstart/init.lua";
    ".config/picom.conf".source = "${configDir}/picom.conf";
    ".config/polybar".source = "${configDir}/polybar";
    ".config/rofi".source = "${configDir}/rofi";
    ".config/tmuxinator".source = "${configDir}/tmuxinator";
    ".config/xmonad".source = "${configDir}/xmonad";
    ".gemrc".source = "${configDir}/gemrc";
    ".gitignore_global".source = "${configDir}/gitignore_global";
    ".i3".source = "${configDir}/i3";
    ".pryrc".source = "${configDir}/pryrc";
    ".tmux/plugins/tmux-sessionx".source = "${configDir}/tmux/plugins/tmux-sessionx";
    ".config/qutebrowser/dracula".source = "${configDir}/qutebrowser/dracula";
    ".config/qutebrowser/config.py".source = "${configDir}/qutebrowser/config.py";
    ".config/qutebrowser/quickmarks".source = "${configDir}/qutebrowser/quickmarks";
  };
}
