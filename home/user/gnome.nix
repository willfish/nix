{ lib, ...}:

with lib.hm.gvariant;
{
  dconf = {
    enable = true;
    settings = {
      "org/gnome/shell/extensions/pop-shell" = {
        activate-launcher = [ "<Super>x" ];
        active-hint = true;
        active-hint-border-radius = "uint32 1";
        focus-down = [ "<Super>s" ];
        focus-left = [ "<Super>a" ];
        focus-right = [ "<Super>d" ];
        focus-up = [ "<Super>w" ];
        gap-inner = "uint32 1";
        gap-outer = "uint32 1";
        show-title = false;
        tile-by-default = true;
        tile-move-down-global = [ "<Shift><Super>s" ];
        tile-move-left-global = [ "<Shift><Super>a" ];
        tile-move-right-global = [ "<Shift><Super>d" ];
        tile-move-up-global = [ "<Shift><Super>w" ];
      };

      "org/gnome/desktop/wm/keybindings" = {
        close = [ "<Super>q" ];
        minimize = [];
        move-to-workspace-1 = [ "<Shift><Super>1" ];
        move-to-workspace-2 = [ "<Shift><Super>2" ];
        move-to-workspace-3 = [ "<Shift><Super>3" ];
        move-to-workspace-4 = [ "<Shift><Super>4" ];
        move-to-workspace-5 = [ "<Shift><Super>5" ];
        move-to-workspace-6 = [ "<Shift><Super>6" ];
        move-to-workspace-7 = [ "<Shift><Super>7" ];
        move-to-workspace-8 = [ "<Shift><Super>8" ];
        move-to-workspace-9 = [ "<Shift><Super>9" ];
        switch-to-workspace-1 = [ "<Super>1" ];
        switch-to-workspace-2 = [ "<Super>2" ];
        switch-to-workspace-3 = [ "<Super>3" ];
        switch-to-workspace-4 = [ "<Super>4" ];
        switch-to-workspace-5 = [ "<Super>5" ];
        switch-to-workspace-6 = [ "<Super>6" ];
        switch-to-workspace-7 = [ "<Super>7" ];
        switch-to-workspace-8 = [ "<Super>8" ];
        switch-to-workspace-9 = [ "<Super>9" ];
        toggle-fullscreen = [ "<Super>f" ];
      };

      "org/gnome/shell/extensions/auto-move-windows" = {
        application-list = [
          "brave-browser.desktop:1"
          "spotify.desktop:3"
          "org.clementine_player.Clementine.desktop:3"
          "slack.desktop:2"
          "org.telegram.desktop.desktop:2"
          "discord.desktop:2"
        ];
      };

      "org/gnome/shell/keybindings" = {
        show-screenshot-ui = [ "<Control><Shift>s" ];
        open-new-window-application-1 = [];
        open-new-window-application-2 = [];
        open-new-window-application-3 = [];
        open-new-window-application-4 = [];
        open-new-window-application-5 = [];
        open-new-window-application-6 = [];
        open-new-window-application-7 = [];
        open-new-window-application-8 = [];
        open-new-window-application-9 = [];
        switch-to-application-1 = [];
        switch-to-application-2 = [];
        switch-to-application-3 = [];
        switch-to-application-4 = [];
        switch-to-application-5 = [];
        switch-to-application-6 = [];
        switch-to-application-7 = [];
        switch-to-application-8 = [];
        switch-to-application-9 = [];
        toggle-application-view = [];
      };

      "org/gnome/shell" = {
        disable-user-extensions = false;
        disabled-extensions = [
          "windowsNavigator@gnome-shell-extensions.gcampax.github.com"
          "system-monitor@gnome-shell-extensions.gcampax.github.com"
          "native-window-placement@gnome-shell-extensions.gcampax.github.com"
          "light-style@gnome-shell-extensions.gcampax.github.com"
          "launch-new-instance@gnome-shell-extensions.gcampax.github.com"
          "apps-menu@gnome-shell-extensions.gcampax.github.com"
          "places-menu@gnome-shell-extensions.gcampax.github.com"
          "drive-menu@gnome-shell-extensions.gcampax.github.com"
          "screenshot-window-sizer@gnome-shell-extensions.gcampax.github.com"
          "workspace-indicator@gnome-shell-extensions.gcampax.github.com"
        ];
        enabled-extensions = [
          "auto-move-windows@gnome-shell-extensions.gcampax.github.com"
          "appindicatorsupport@rgcjonas.gmail.com"
          "pop-shell@system76.com"
          "status-icons@gnome-shell-extensions.gcampax.github.com"
        ];
        welcome-dialog-last-shown-version = "45.5";
      };

      "org/gnome/clocks/state/window" = {
        maximized = false;
        panel-id = "world";
        size = mkTuple [ 850 1392 ];
      };

      "org/gnome/desktop/input-sources" = {
        sources = [ (mkTuple [ "xkb" "us" ]) ];
        xkb-options = [ "terminate:ctrl_alt_bksp" ];
      };

      "org/gnome/desktop/peripherals/touchpad" = {
        two-finger-scrolling-enabled = true;
      };

      "org/gnome/desktop/search-providers" = {
        enabled = ["org.gnome.Weather.desktop"];
        sort-order = [
          "org.gnome.Settings.desktop"
          "org.gnome.Contacts.desktop"
          "org.gnome.Nautilus.desktop"
        ];
      };

      "org/gnome/desktop/session" = {
        idle-delay = mkUint32 600;
      };

      "org/gnome/desktop/wm/preferences" = {
        auto-raise = false;
        button-layout = "appmenu:close";
        focus-mode = "mouse";
        num-workspaces = 6;
      };

      "org/gnome/mutter" = {
        dynamic-workspaces = false;
        edge-tiling = false;
        overlay-key = "Super_L";
      };

      "org/gnome/shell/extensions/workspace-indicator" = {
        embed-previews = false;
      };
    };
  };
}
