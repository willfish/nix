{ pkgs-unstable, ... }:
{
  system.configurationSpecialisation = {
    i3 = { config, pkgs, ... }: {
      services.xserver = {
        xkb.layout = "us";
        xkb.variant = "";
        enable = true;
        windowManager.i3 = {
          enable = true;
          extraPackages = with pkgs-unstable; [
            dconf
            dmenu
            dunst
            feh
            gexiv2
            gtk3
            i3blocks
            i3lock-blur
            i3status
            imagemagick
            libayatana-indicator-gtk3
            libnotify
            maim
            networkmanagerapplet
            nitrogen
            pa_applet
            picom
            polybar
            rofi
            swappy
            xautolock
          ];
        };
      };
    };

    xmonad = { config, pkgs, ... }: {
      services.xserver = {
        enable = true;
        xkb.layout = "us";
        xkb.variant = "";
        windowManager.xmonad = {
          enable = true;
          config = ''
            import XMonad
            import XMonad.Layout.Spacing

            main = xmonad $ def {
              terminal = "ghostty",
              modMask  = mod4Mask,
              layoutHook = spacing 8 $ Tall 1 (3/100) (1/2) ||| Full
            }
          '';
        };
      };

      environment.systemPackages = with pkgs; [
        xmonad
        xmonad-contrib
        xmobar
      ];
    };
  };

}
