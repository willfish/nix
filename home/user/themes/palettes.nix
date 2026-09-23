# Upstream native themes projected onto the shared Base16 application roles.
let
  source = import ./omarchy-source.nix;
  labels = {
    catppuccin = "Catppuccin";
    catppuccin-latte = "Catppuccin Latte";
    ethereal = "Ethereal";
    everforest = "Everforest";
    flexoki-light = "Flexoki Light";
    gruvbox = "Gruvbox";
    hackerman = "Hackerman";
    kanagawa = "Kanagawa";
    last-horizon = "Last Horizon";
    lumon = "Lumon";
    lupine = "Lupine";
    matte-black = "Matte Black";
    miasma = "Miasma";
    nord = "Nord";
    osaka-jade = "Osaka Jade";
    retro-82 = "Retro 82";
    ristretto = "Ristretto";
    rose-pine = "Rosé Pine";
    solitude = "Solitude";
    tokyo-night = "Tokyo Night";
    vantablack = "Vantablack";
    white = "White";
  };
  names = builtins.filter (name: builtins.pathExists "${source}/themes/${name}/colors.toml") (
    builtins.attrNames (builtins.readDir "${source}/themes")
  );
  theme =
    name:
    let
      colours = builtins.fromTOML (builtins.readFile "${source}/themes/${name}/colors.toml");
      # Match upstream's optional orange/brown defaults: yellow and a 50%
      # black mix, rounding each channel to the nearest integer.
      orange = colours.orange or colours.yellow;
      brown =
        "#"
        + builtins.concatStringsSep "" (
          map
            (
              offset:
              let
                channel = (builtins.fromTOML "value = 0x${builtins.substring offset 2 orange}").value;
                half = builtins.div (channel + 1) 2;
                high = builtins.div half 16;
                digits = "0123456789abcdef";
              in
              builtins.substring high 1 digits + builtins.substring (half - high * 16) 1 digits
            )
            [
              1
              3
              5
            ]
        );
      defaults = { inherit orange brown; };
      colour = field: builtins.substring 1 6 (colours.${field} or defaults.${field});
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
    in
    {
      inherit name;
      label = labels.${name} or name;
      nativeMode = colours.mode;
      # Compatibility slots for paired application/greeter renderers, not
      # invented variants. The menu applies only the upstream native mode.
      dark = palette;
      light = palette;
      herdr = {
        inherit name;
        auto_switch = true;
        dark_name = name;
        light_name = name;
      };
    };
in
builtins.listToAttrs (
  map (name: {
    inherit name;
    value = theme name;
  }) names
)
