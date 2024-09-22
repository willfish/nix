{ config, ... }:
let
  configDir = ../config;

  configure = directory:
    config.lib.file.mkOutOfStoreSymlink "${configDir}/${directory}";
in
{
  home.file = {
      ".config/btop".source = "${configDir}/btop";
      ".config/hypr".source = "${configDir}/hypr";
      ".config/kitty".source = "${configDir}/kitty";
      ".config/mako".source = "${configDir}/mako";
      ".config/nvim".source = configure "nvim";
      ".config/picom.conf".source = "${configDir}/picom.conf";
      ".config/polybar".source = "${configDir}/polybar";
      ".config/rofi".source = "${configDir}/rofi";
      ".config/swayidle".source = "${configDir}/swayidle";
      ".config/swaylock".source = "${configDir}/swaylock";
      ".config/waybar".source = "${configDir}/waybar";
      ".config/wlogout".source = "${configDir}/wlogout";
      ".config/wofi".source = "${configDir}/wofi";
      ".i3".source = "${configDir}/i3";
      ".tmux/plugins/tmux-sessionx".source = "${configDir}/tmux/plugins/tmux-sessionx";
      ".wallpapers".source = "${configDir}/variety/Favorites";
  };
}
