{ lib }:
let
  source = import ./omarchy-source.nix;
  catalogue = import ./palettes.nix;
  mkTheme = import ./mk-theme.nix { inherit lib; };
  packages = lib.mapAttrs (_: mkTheme) catalogue;
  # These themes sort a solid-colour image before their illustrated wallpapers.
  selection =
    theme:
    let
      preferredName =
        {
          community-midnight = "2-hand-of-adam.png";
          community-oxo-carbon = "BG3.jpg";
        }
        .${theme.name} or "";
      wallpaper =
        if theme.backgrounds == [ ] then
          null
        else
          lib.findFirst (
            file: builtins.baseNameOf file == preferredName
          ) (builtins.head theme.backgrounds) theme.backgrounds;
    in
    {
      backgrounds = map builtins.baseNameOf theme.backgrounds;
      preferred = if wallpaper == null then null else builtins.baseNameOf wallpaper;
      wallpaperColor = theme.${theme.nativeMode or "dark"}.base00;
    };
in
{
  inherit packages selection;
  license = "${source}/LICENSE";
}
