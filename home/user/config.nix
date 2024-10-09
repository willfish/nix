{ config, ... }:
let
  configDir = ../config;

  configure = directory:
    config.lib.file.mkOutOfStoreSymlink "${configDir}/${directory}";
in
{
  home.file = {
      ".aprc".source = "${configDir}/aprc";
      ".bin/".source = "${configDir}/bin";
      ".config/btop".source = "${configDir}/btop";
      ".config/kitty".source = "${configDir}/kitty";
      ".config/nvim".source = configure "nvim";
      ".config/picom.conf".source = "${configDir}/picom.conf";
      ".config/polybar".source = "${configDir}/polybar";
      ".config/rofi".source = "${configDir}/rofi";
      ".gemrc".source = "${configDir}/gemrc";
      ".gitignore_global".source = "${configDir}/gitignore_global";
      ".i3".source = "${configDir}/i3";
      ".pryrc".source = "${configDir}/pryrc";
      ".tmux/plugins/tmux-sessionx".source = "${configDir}/tmux/plugins/tmux-sessionx";
      ".wallpapers".source = "${configDir}/variety/Favorites";
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
