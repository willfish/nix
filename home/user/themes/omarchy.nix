{ lib, pkgs }:
let
  source = import ./omarchy-source.nix;
  catalogue = import ./palettes.nix;
  mkTheme = import ./mk-theme.nix {
    inherit lib pkgs;
    fallbackUnlock = "${source}/default/plymouth/logo.png";
    fallbackLicense = "${source}/LICENSE";
  };
  packages = lib.mapAttrs (_: mkTheme) catalogue;
in
{
  inherit packages;
  wallpapers = lib.mapAttrs (_: package: "${package}/wallpaper.png") packages;
  license = "${source}/LICENSE";
}
