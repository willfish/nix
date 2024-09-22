{ config, ... }:
let
  configDir = ../config;

  configure = directory:
    config.lib.file.mkOutOfStoreSymlink "${configDir}/${directory}";
in
{
  home.file = {
      ".config/btop".source = "${configDir}/btop";
      ".config/kitty".source = "${configDir}/kitty";
      ".config/nvim".source = configure "nvim";
      ".config/picom.conf".source = "${configDir}/picom.conf";
      ".config/polybar".source = "${configDir}/polybar";
      ".config/rofi".source = "${configDir}/rofi";
      ".i3".source = "${configDir}/i3";
      ".tmux/plugins/tmux-sessionx".source = "${configDir}/tmux/plugins/tmux-sessionx";
      ".wallpapers".source = "${configDir}/variety/Favorites";
      ".bin/random-wallpaper".source = "${configDir}/random-wallpaper.sh";
      ".gemrc".source = "${configDir}/gemrc";
      ".pryrc".source = "${configDir}/pryrc";
      ".aprc".source = "${configDir}/aprc";
  };

  systemd.user.timers.wallpaper = {
      Unit = {
          Description = "Random wallpaper scheduler";
      };

      Timer = {
          OnBootSec = "20s";
          OnUnitActiveSec = "20s";
          Unit = "wallpaper.service";
      };
  };


  systemd.user.services.wallpaper = {
      Unit = {
          Description = "Random wallpaper";
      };
      Install = {
          WantedBy = [ "default.target" ];
      };
      Service = {
          Type = "oneshot";
          ExecStart="${configDir}/random-wallpaper.sh";
          Environment = [
              "\"PATH=/run/current-system/sw/bin\""
          ];
      };
  };
}
