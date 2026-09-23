# btop theme for one palette. A theme that ships btop.theme keeps that file;
# every other theme is rendered from the shared colour roles, with upstream
# blue kept distinct from accent.
{ lib, pkgs }:
let
  source = import ./omarchy-source.nix;
  render = import ./render.nix { inherit lib; };
in
name: palette:
let
  custom = "${source}/themes/${name}/btop.theme";
  colours = builtins.fromTOML (builtins.readFile "${source}/themes/${name}/colors.toml");
  blue = lib.removePrefix "#" (colours.blue or ("#" + palette.base0D));
in
if builtins.pathExists custom then
  custom
else
  pkgs.writeText "btop-${name}.theme" (render.btop (palette // { inherit blue; }))
