# Import Omarchy's data format, never a repository's installation scripts.
{
  name,
  source,
  displayName ? name,
  licenseSource ? source,
}:
let
  files = builtins.readDir source;
  regular = file: (files.${file} or null) == "regular";
  colours =
    if regular "colors.toml" then
      builtins.fromTOML (builtins.readFile "${source}/colors.toml")
    else
      throw "Theme ${name}: a regular colors.toml is required (legacy script-only themes are unsupported)";
  hex =
    value:
    if builtins.isString value && builtins.match "#[0-9a-fA-F]{6}" value != null then
      builtins.substring 1 6 value
    else
      throw "Theme ${name}: expected a six-digit #RRGGBB colour";
  orange = colours.orange or colours.yellow;
  brown =
    "#"
    + builtins.concatStringsSep "" (
      map
        (
          offset:
          let
            channel = (builtins.fromTOML "value = 0x${builtins.substring offset 2 (hex orange)}").value;
            half = builtins.div (channel + 1) 2;
            high = builtins.div half 16;
            digits = "0123456789abcdef";
          in
          builtins.substring high 1 digits + builtins.substring (half - high * 16) 1 digits
        )
        [
          0
          2
          4
        ]
    );
  defaults = { inherit orange brown; };
  colour = field: hex (colours.${field} or defaults.${field});
  palette = {
    base00 = colour "background";
    base01 = colour "lighter_background";
    base02 = colour "selection";
    base03 = colour "muted";
    base04 = colour "dark_foreground";
    base05 = colour "foreground";
    base06 = colour "light_foreground";
    base07 = colour "bright_foreground";
    base08 = colour "red";
    base09 = colour "orange";
    base0A = colour "yellow";
    base0B = colour "green";
    base0C = colour "cyan";
    base0D = colour "accent";
    base0E = colour "magenta";
    base0F = colour "brown";
  };
  backgroundFiles =
    if (files.backgrounds or null) == "directory" then
      builtins.readDir "${source}/backgrounds"
    else
      { };
  images = builtins.filter (
    file:
    backgroundFiles.${file} == "regular"
    && builtins.match ".*\\.(png|jpg|jpeg|webp|PNG|JPG|JPEG|WEBP)" file != null
  ) (builtins.attrNames backgroundFiles);
  licenseFiles = builtins.readDir licenseSource;
  licenses = builtins.filter (
    file:
    licenseFiles.${file} == "regular" && builtins.match "(LICENSE|COPYING|NOTICE)(\\..*)?" file != null
  ) (builtins.attrNames licenseFiles);
in
# The login bridge permits 64 bytes including the trailing newline.
assert builtins.match "[a-z0-9]+(-[a-z0-9]+)*" name != null && builtins.stringLength name <= 63;
assert (
  if
    builtins.elem (colours.mode or null) [
      "light"
      "dark"
    ]
  then
    true
  else
    throw "Theme ${name}: colors.toml mode must be light or dark"
);
builtins.deepSeq palette {
  inherit
    name
    source
    colours
    palette
    ;
  label = builtins.replaceStrings [ "-" ] [ " " ] displayName;
  nativeMode = colours.mode;
  backgrounds = map (file: "${source}/backgrounds/${file}") images;
  unlock = if regular "unlock.png" then "${source}/unlock.png" else null;
  btop = if regular "btop.theme" then "${source}/btop.theme" else null;
  licenses = map (file: "${licenseSource}/${file}") licenses;
  # Compatibility slots, both containing the native palette, not invented modes.
  dark = palette;
  light = palette;
  herdr = {
    inherit name;
    auto_switch = true;
    dark_name = name;
    light_name = name;
  };
}
