# Build every greeter theme's formats.toml output without wallpaper builds.
# String context is discarded only in this test bundle. The system module
# keeps the context so the wallpapers stay reachable from the config.
let
  flake = builtins.getFlake (toString ../.);
  pkgs = import flake.inputs.nixpkgs {
    system = "x86_64-linux";
    config.allowUnfree = true;
  };
  inherit (pkgs) lib;
  rendered = import ../system/modules/greeter-themes.nix {
    inherit lib pkgs;
    timeZone = "Europe/London";
  };
  dropContext =
    value:
    if lib.isString value then
      builtins.unsafeDiscardStringContext value
    else if lib.isAttrs value then
      lib.mapAttrs (_: dropContext) value
    else if lib.isList value then
      map dropContext value
    else
      value;
  format = pkgs.formats.toml { };
  files = lib.mapAttrs (
    name: _: format.generate "regreet-${name}.toml" (dropContext (rendered.settingsFor name))
  ) rendered.catalogue;
in
pkgs.linkFarm "greeter-theme-toml" (
  lib.mapAttrsToList (name: path: {
    name = "${name}.toml";
    inherit path;
  }) files
)
