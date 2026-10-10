# btop theme text for one palette. A theme that ships btop.theme keeps that
# file; every other theme is rendered from the shared colour roles, with
# upstream blue kept distinct from accent. Returns text, not a derivation, so
# the result does not follow nixpkgs.
{ lib }:
let
  catalogue = import ./palettes.nix;
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
  # Copy the text only. Referencing the theme tree would root its wallpapers.
  builtins.readFile custom
else
  render.btop (palette // { inherit blue; })
