# Host default theme IDs for the menu and the greeter. IDs must exist in
# palettes.nix. Unknown hosts use fallback. Null or empty means andromeda.
let
  catalogue = import ./palettes.nix;
  defaults = {
    andromeda = "rose-pine";
    foundation = "tokyo-night";
    starfish = "osaka-jade";
    terminus = "catppuccin";
    relay = "gruvbox";
  };
  fallback = "rose-pine";
  known = id: builtins.hasAttr id catalogue;
  bad = builtins.filter (host: !known defaults.${host}) (builtins.attrNames defaults);
in
if bad != [ ] then
  throw "Unknown host default theme IDs: ${builtins.concatStringsSep ", " bad}"
else if !known fallback then
  throw "Unknown fallback theme ID: ${fallback}"
else
  {
    inherit defaults fallback;
    forHost =
      hostName:
      let
        key = if hostName == null || hostName == "" then "andromeda" else hostName;
        id = defaults.${key} or fallback;
      in
      if known id then id else throw "Unknown theme ID for ${key}: ${id}";
  }
