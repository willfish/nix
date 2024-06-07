{ config, ... }:
let
  configDir = ../config;

  configure = directory:
    config.lib.file.mkOutOfStoreSymlink "${configDir}/${directory}";
in
{
  home.file = {
      ".config/nvim".source = configure "nvim";
      ".config/kitty".source = "${configDir}/kitty";
      ".config/hypr".source = "${configDir}/hypr";
      ".config/swayidle".source = "${configDir}/swayidle";
      ".config/swaylock".source = "${configDir}/swaylock";
      ".config/wlogout".source = "${configDir}/wlogout";
      ".config/waybar".source = "${configDir}/waybar";
      ".config/btop".source = "${configDir}/btop";
      ".config/wofi".source = "${configDir}/wofi";
      ".config/mako".source = "${configDir}/mako";
      ".i3".source = "${configDir}/i3";
      ".config/polybar".source = "${configDir}/polybar";
      ".config/rofi".source = "${configDir}/rofi";
      ".config/picom.conf".source = "${configDir}/picom.conf";
      ".wallpapers".source = "${configDir}/variety/Favorites";
  };
}
