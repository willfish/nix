{
  config,
  lib,
  pkgs,
  ...
}:
let
  inherit (import ../../home/config/hyprland/settings.nix) appearance;
  defaults = import ../../home/user/themes/host-defaults.nix;
  theme =
    if appearance.bootPalette == null then
      defaults.forHost config.networking.hostName
    else
      appearance.bootPalette;
  rendered = import ./greeter-themes.nix { inherit lib pkgs; };
in
{
  assertions = [
    {
      assertion = rendered.catalogue ? ${theme};
      message = "appearance.bootPalette must name an available theme";
    }
  ];
  # Boot artwork is embedded in the initrd. Never read a user's runtime
  # selection during evaluation, and leave encryption/unlock policy intact.
  boot.plymouth = {
    enable = true;
    theme = "omarchy";
    themePackages = [ (rendered.plymouthPackage theme) ];
  };
}
