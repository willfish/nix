# btop theme for one palette. A theme that ships btop.theme keeps that file;
# every other theme is rendered from the shared colour roles, with upstream
# blue kept distinct from accent.
{ lib, pkgs }:
let
  catalogue = import ./palettes.nix;
  themes = import ./omarchy.nix { inherit lib pkgs; };
  render = import ./render.nix { inherit lib; };
in
name: palette:
let
  inherit (catalogue.${name}) colours;
  custom = catalogue.${name}.btop;
  blueValue = colours.blue or ("#" + palette.base0D);
  blue =
    assert lib.assertMsg (
      builtins.isString blueValue && builtins.match "#[0-9a-fA-F]{6}" blueValue != null
    ) "Theme ${name}: blue must be a six-digit #RRGGBB colour";
    lib.removePrefix "#" blueValue;
in
if custom != null then
  "${themes.packages.${name}}/btop.theme"
else
  pkgs.writeText "btop-${name}.theme" (render.btop (palette // { inherit blue; }))
