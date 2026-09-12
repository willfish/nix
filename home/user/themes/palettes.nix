# Base16 roles: background, surface, selection, border, muted text, text,
# strong text, inverse text, red, orange, yellow, green, teal, accent, purple, rose.
# Light variants intentionally use warm paper rather than the stock white/grey.
let
  keys = [
    "base00"
    "base01"
    "base02"
    "base03"
    "base04"
    "base05"
    "base06"
    "base07"
    "base08"
    "base09"
    "base0A"
    "base0B"
    "base0C"
    "base0D"
    "base0E"
    "base0F"
  ];
  palette =
    colours:
    builtins.listToAttrs (
      builtins.genList (i: {
        name = builtins.elemAt keys i;
        value = builtins.elemAt colours i;
      }) 16
    );
  theme = darkName: lightName: dark: light: {
    herdr = {
      name = darkName;
      auto_switch = true;
      dark_name = darkName;
      light_name = lightName;
    };
    dark = palette dark;
    light = palette light;
  };
in
{
  andromeda =
    theme "rose-pine" "rose-pine-dawn"
      [
        "191724"
        "1f1d2e"
        "26233a"
        "524f67"
        "a7a3bf"
        "e0def4"
        "e0def4"
        "e0def4"
        "eb6f92"
        "ebbcba"
        "f6c177"
        "9ccfd8"
        "9ccfd8"
        "c4a7e7"
        "c4a7e7"
        "ebbcba"
      ]
      [
        "f3ebdf"
        "ece2d4"
        "e5d8c8"
        "9a8c86"
        "685960"
        "484052"
        "393340"
        "302b35"
        "a83252"
        "8f4a25"
        "795516"
        "34685d"
        "286477"
        "714d87"
        "714d87"
        "94435c"
      ];
  foundation =
    theme "tokyo-night" "tokyo-night-day"
      [
        "1a1b26"
        "1f2335"
        "292e42"
        "414868"
        "a9b1d6"
        "c0caf5"
        "d5ddfa"
        "d5ddfa"
        "f7768e"
        "ff9e64"
        "e0af68"
        "9ece6a"
        "7dcfff"
        "7aa2f7"
        "bb9af7"
        "c099ff"
      ]
      [
        "f2eadc"
        "ebe1d2"
        "e4d7c5"
        "998d7f"
        "615a61"
        "343b58"
        "292f47"
        "292f47"
        "a63249"
        "8f4e20"
        "765614"
        "456523"
        "236777"
        "305b9f"
        "704da0"
        "844870"
      ];
  starfish =
    theme "solarized" "solarized-light"
      [
        "002b36"
        "073642"
        "103e49"
        "586e75"
        "a3b1b1"
        "eee8d5"
        "fdf6e3"
        "fdf6e3"
        "f98279"
        "e69a62"
        "d9b54a"
        "b4c75e"
        "6cc7be"
        "66b6e8"
        "c59bd5"
        "e795bd"
      ]
      [
        "f3ecd9"
        "eae2ce"
        "e2d7bf"
        "998e75"
        "625b4c"
        "354b50"
        "293c40"
        "293c40"
        "a73132"
        "904811"
        "785814"
        "50641a"
        "226961"
        "20637e"
        "6c518a"
        "97375f"
      ];
  terminus =
    theme "catppuccin" "catppuccin-latte"
      [
        "1e1e2e"
        "242438"
        "313244"
        "585b70"
        "bac2de"
        "cdd6f4"
        "e2e7fa"
        "e2e7fa"
        "f38ba8"
        "fab387"
        "f9e2af"
        "a6e3a1"
        "94e2d5"
        "b4befe"
        "cba6f7"
        "f5c2e7"
      ]
      [
        "f2eadd"
        "eae0d1"
        "e3d6c5"
        "978b83"
        "625a60"
        "454456"
        "353442"
        "353442"
        "ad2448"
        "8d491b"
        "795516"
        "3c692a"
        "21695e"
        "5c529c"
        "7547a0"
        "89446a"
      ];
  relay =
    theme "gruvbox" "gruvbox-light"
      [
        "282828"
        "30302e"
        "3c3836"
        "665c54"
        "bdae93"
        "ebdbb2"
        "fbf1c7"
        "fbf1c7"
        "fb8575"
        "feaa62"
        "fabd2f"
        "b8bb26"
        "8ec07c"
        "83b8a0"
        "dd90a5"
        "d5a98b"
      ]
      [
        "f2e7ce"
        "eaddc1"
        "e1d3b4"
        "988870"
        "665847"
        "483d32"
        "362e26"
        "362e26"
        "a42a20"
        "914711"
        "76540e"
        "516219"
        "346452"
        "306350"
        "854665"
        "824a2d"
      ];
}
