# Data-only equivalents of Omarchy's theme-color and colors-from-alacritty
# compatibility rules. Do not load terminal settings or execute theme scripts.
{ name, source }:
let
  files = builtins.readDir source;
  regular = file: (files.${file} or null) == "regular";
  toml = file: builtins.fromTOML (builtins.readFile "${source}/${file}");
  alacritty = (toml "alacritty.toml").colors;
  inherit (alacritty) normal;
  bright = alacritty.bright or { };
  primary = alacritty.primary or { };
  # Historical Alacritty palettes also permit bare or 0x-prefixed RGB strings.
  terminalHex =
    value:
    if builtins.isString value then
      let
        match = builtins.match "(#|0[xX])?([0-9a-fA-F]{6})" value;
      in
      if match != null then
        "#${builtins.elemAt match 1}"
      else
        throw "Theme ${name}: invalid Alacritty RGB colour"
    else
      throw "Theme ${name}: expected a string Alacritty RGB colour";
  ansiNames = [
    "black"
    "red"
    "green"
    "yellow"
    "blue"
    "magenta"
    "cyan"
    "white"
  ];
  fromTerminal = builtins.mapAttrs (_: terminalHex) (
    builtins.listToAttrs (
      builtins.genList (index: {
        name = "color${toString index}";
        value =
          let
            key = builtins.elemAt ansiNames (index - builtins.div index 8 * 8);
          in
          if index < 8 then normal.${key} else bright.${key} or normal.${key};
      }) 16
    )
    // {
      background = primary.background or normal.black;
      foreground = primary.foreground or normal.white;
      selection =
        let
          value = alacritty.selection.background or "";
        in
        if builtins.isString value && builtins.match "(#|0[xX])?[0-9a-fA-F]{6}" value != null then
          value
        else
          primary.foreground or normal.white;
      accent = normal.blue;
    }
  );
  raw =
    if regular "colors.toml" then
      toml "colors.toml"
    else if regular "alacritty.toml" then
      fromTerminal
    else
      throw "Theme ${name}: requires regular colors.toml or alacritty.toml palette data";
  # Prefer canonical semantic names, then historical aliases, then ANSI slots.
  pick =
    keys: fallback:
    let
      present = builtins.filter (key: builtins.hasAttr key raw && raw.${key} != "") keys;
    in
    if present == [ ] then fallback else raw.${builtins.head present};
  missing = field: throw "Theme ${name}: missing ${field} colour";
  background = pick [ "background" "bg" "color0" ] (missing "background");
  foreground = pick [ "foreground" "fg" "color7" ] (missing "foreground");
  channel =
    offset:
    if builtins.isString background && builtins.match "#[0-9a-fA-F]{6}" background != null then
      (builtins.fromTOML "value = 0x${builtins.substring offset 2 background}").value
    else
      throw "Theme ${name}: invalid background RGB colour";
  detectedMode =
    if regular "light.mode" || channel 1 + channel 3 + channel 5 > 382 then "light" else "dark";
  colours = {
    inherit background foreground;
    mode = pick [ "mode" "theme_type" ] detectedMode;
    lighter_background = pick [ "lighter_background" "lighter_bg" ] background;
    dark_foreground = pick [ "dark_foreground" "dark_fg" "color8" ] foreground;
    light_foreground = pick [ "light_foreground" "light_fg" ] foreground;
    bright_foreground = pick [ "bright_foreground" "bright_fg" "color15" ] foreground;
    muted = pick [ "muted" "color8" ] colours.dark_foreground;
    selection = pick [ "selection" "selection_background" "color8" ] background;
    red = pick [ "red" "color1" ] (missing "red");
    green = pick [ "green" "color2" ] (missing "green");
    yellow = pick [ "yellow" "color3" ] (missing "yellow");
    blue = pick [ "blue" "color4" ] (missing "blue");
    magenta = pick [ "magenta" "color5" "purple" ] (missing "magenta");
    cyan = pick [ "cyan" "color6" ] (missing "cyan");
    accent = pick [ "accent" ] colours.blue;
    orange = pick [ "orange" ] colours.yellow;
  }
  // (if raw ? brown then { inherit (raw) brown; } else { });
in
colours
