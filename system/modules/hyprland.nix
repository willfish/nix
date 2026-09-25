{ pkgs, ... }:
{
  programs.hyprland.enable = true;
  # Region capture uses the KMS helper. The wrapper is what makes that promptless.
  programs.gpu-screen-recorder.enable = true;
  security.pam.services.hyprlock = { };

  xdg.portal = {
    extraPortals = [ pkgs.xdg-desktop-portal-gtk ];
    config.hyprland = {
      default = [
        "hyprland"
        "gtk"
      ];
      "org.freedesktop.impl.portal.FileChooser" = [ "gtk" ];
      "org.freedesktop.impl.portal.Settings" = [ "gtk" ];
    };
  };
}
