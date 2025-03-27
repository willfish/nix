{ pkgs-unstable, ...}:
{
  dconf = {
    enable = true;
    settings = {
      "org/gnome/shell" = {
        disable-user-extensions = false;
        enabled-extensions = with pkgs-unstable; [
          gnomeExtensions.auto-move-windows.extensionUuid # GNOME extension for automatic window positioning
          gnomeExtensions.appindicator.extensionUuid      # GNOME extension for app indicators
          gnomeExtensions.pop-shell.extensionUuid         # GNOME extension for tiling window management
          gnomeExtensions.status-icons.extensionUuid      # GNOME extension for status icons
        ];
      };

      "org/gnome/desktop/wm/keybindings" = {
        minimize = [];
        close = ["<Super>q"];
        toggle-fullscreen = ["<Super>f"];
        move-to-workspace-1 = ["<Shift><Super>1"];
        move-to-workspace-2 = ["<Shift><Super>2"];
        move-to-workspace-3 = ["<Shift><Super>3"];
        move-to-workspace-4 = ["<Shift><Super>4"];
        switch-to-workspace-1 = ["<Super>1"];
        switch-to-workspace-2 = ["<Super>2"];
        switch-to-workspace-3 = ["<Super>3"];
        switch-to-workspace-4 = ["<Super>4"];
      };
      "org/gnome/desktop/wm/preferences" = {
        focus-mode = "mouse";
        num-workspaces=6;
      };

      "org/gnome/mutter" = {
        dynamic-workspaces=false;
        edge-tiling=false;
      };

"org/gnome/settings-daemon/plugins/color" = {
          night-light-enabled=true;
          night-light-schedule-automatic=false;
          night-light-temperature="uint32 3482";
        };

      "org/gnome/shell/extensions/auto-move-windows" = {
        application-list=[
          "brave-browser.desktop:1" "spotify.desktop:3" "org.clementine_player.Clementine.desktop:3" "slack.desktop:2" "org.telegram.desktop.desktop:2" "discord.desktop:2"
        ];
      };

      "org/gnome/shell/extensions/pop-shell" = {
        active-hint = true;
        active-hint-border-radius = "uint32 1";
        gap-inner = "uint32 1";
        gap-outer = "uint32 1";
        show-title = false;
        tile-by-default = true;
        focus-left = ["<Super>a"];
        focus-down = ["<Super>s"];
        focus-up = ["<Super>w"];
        focus-right = ["<Super>d"];
        activate-launcher = ["<Super>x"];
        tile-move-left-global = ["<Shift><Super>a"];
        tile-move-down-global = ["<Shift><Super>s"];
        tile-move-up-global = ["<Shift><Super>w"];
        tile-move-right-global = ["<Shift><Super>d"];
      };

      "org/gnome/shell/keybindings" = {
        show-screenshot-ui=["<Control><Shift>s"];
        switch-to-application-1="@as []";
        switch-to-application-2="@as []";
        switch-to-application-3="@as []";
        switch-to-application-4="@as []";
      };
    };
  };
}
