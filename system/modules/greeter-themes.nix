{
  lib,
  pkgs,
  timeZone,
}:
let
  catalogue = import ../../home/user/themes/palettes.nix;
  omarchy = import ../../home/user/themes/omarchy.nix { inherit lib pkgs; };
  desktopTheme = import ../../home/user/themes/hyprland.nix { inherit lib; };
  configuredAppearance = (import ../../home/config/hyprland/settings.nix).appearance;
  gtkThemeName = mode: if mode == "dark" then "adw-gtk3-dark" else "adw-gtk3";
  applyOverrides =
    theme:
    let
      mode = theme.nativeMode;
      overrides = configuredAppearance.paletteOverrides.${mode} or { };
      base = theme.${mode};
      valid = lib.all (
        key:
        builtins.hasAttr key base
        && builtins.isString overrides.${key}
        && builtins.match "[0-9a-fA-F]{6}" overrides.${key} != null
      ) (lib.attrNames overrides);
    in
    assert lib.assertMsg (
      mode == "dark" || mode == "light"
    ) "theme ${theme.name} nativeMode must be dark or light";
    assert lib.assertMsg valid
      "paletteOverrides.${mode} must contain Base16 keys and six-digit hex colours";
    base // overrides;
  settingsFor = theme: {
    background = {
      path = "${omarchy.wallpapers.${theme.name}}";
      fit = "Cover";
    };
    GTK = {
      application_prefer_dark_theme = theme.nativeMode == "dark";
      cursor_theme_name = "Adwaita";
      font_name = "${configuredAppearance.font} ${toString configuredAppearance.fontSize}";
      icon_theme_name = "Adwaita";
      theme_name = gtkThemeName theme.nativeMode;
    };
    commands = {
      reboot = [
        "${pkgs.systemd}/bin/systemctl"
        "reboot"
      ];
      poweroff = [
        "${pkgs.systemd}/bin/systemctl"
        "poweroff"
      ];
    };
    widget.clock = {
      format = "%a %H:%M";
      resolution = "500ms";
      timezone = timeZone;
    };
  };
  cssText =
    theme:
    (desktopTheme.render {
      mode = theme.nativeMode;
      palette = applyOverrides theme;
      appearance = configuredAppearance;
    })."gtk.css";
  format = pkgs.formats.toml { };
in
{
  inherit catalogue configuredAppearance gtkThemeName;
  names = lib.attrNames catalogue;
  settingsFor = name: settingsFor catalogue.${name};
  cssText = name: cssText catalogue.${name};
  nativeMode = name: catalogue.${name}.nativeMode;
  configFile = name: format.generate "regreet-${name}.toml" (settingsFor catalogue.${name});
  cssFile = name: pkgs.writeText "regreet-${name}.css" (cssText catalogue.${name});
}
