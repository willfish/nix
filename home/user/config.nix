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
    ".config/nvim/init.lua".source = "${configDir}/nvim/init.lua";
    ".config/picom.conf".source = "${configDir}/picom.conf";
    ".config/polybar".source = "${configDir}/polybar";
    ".config/rofi".source = "${configDir}/rofi";
    ".config/tmuxinator".source = "${configDir}/tmuxinator";
    ".config/xmonad".source = "${configDir}/xmonad";
    ".gemrc".source = "${configDir}/gemrc";
    ".gitmessage".source = "${configDir}/gitmessage";
    ".gitignore_global".source = "${configDir}/gitignore_global";
    ".pryrc".source = "${configDir}/pryrc";
    ".tmux/plugins/tmux-sessionx".source = "${configDir}/tmux/plugins/tmux-sessionx";
  };
}
