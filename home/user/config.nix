{ config, lib, ... }:
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
  };
}
