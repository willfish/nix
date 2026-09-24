# Herdr rejects theme names outside its built-in list. Palette identity stays on
# theme.herdr.name for the menu, wallpapers and btop. The generated config uses
# a built-in base; custom colours still carry the selected palette.
_:
let
  accepted = [
    "catppuccin"
    "catppuccin-latte"
    "terminal"
    "tokyo-night"
    "tokyo-night-day"
    "dracula"
    "nord"
    "gruvbox"
    "gruvbox-light"
    "one-dark"
    "one-light"
    "solarized"
    "solarized-light"
    "kanagawa"
    "kanagawa-lotus"
    "rose-pine"
    "rose-pine-dawn"
    "vesper"
  ];
in
{
  configTheme =
    theme: custom:
    let
      id = theme.herdr.name;
      base =
        if builtins.elem id accepted then
          id
        else if (theme.nativeMode or "dark") == "light" then
          "catppuccin-latte"
        else
          "catppuccin";
    in
    theme.herdr
    // {
      name = base;
      dark_name = base;
      light_name = base;
      inherit custom;
    };
}
