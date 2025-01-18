{ config, pkgs-unstable, ... }:
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
      ".config/ghostty/config".source = "${configDir}/ghostty/config";
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
          ExecStart="${configDir}/bin/random-wallpaper";
          Environment = [
              "\"PATH=/run/current-system/sw/bin\""
          ];
      };
  };
  systemd.user.services.connectBluetoothSpeaker = {
    Unit = {
      Description = "Connect my BT speaker on user login";
      After = [ "default.target" "suspend.target" "hibernate.target" "hybrid-sleep.target" "bluetooth.service" ];
    };
    Service = {
      Type = "oneshot";
      ExecStart = "${pkgs-unstable.bluez}/bin/bluetoothctl connect AC:A9:B4:00:0E:21";
    };
    Install = {
      WantedBy = [ "default.target" ];
    };
  };

  systemd.user.startServices = "sd-switch";
}
