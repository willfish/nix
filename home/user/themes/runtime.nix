{
  config,
  lib,
  pkgs,
  catalogue,
  defaultHost,
}:
let
  state = "${config.xdg.stateHome}/theme-menu";
  render = import ./render.nix { inherit lib; };
  hyprland = import ./hyprland.nix { inherit lib; };
  omarchy = import ./omarchy.nix { inherit lib pkgs; };
  wallpaperSettings = (import ../../config/hyprland/settings.nix).wallpaper;
  configuredAppearance = (import ../../config/hyprland/settings.nix).appearance;
  preferredPalette = configuredAppearance.palette;
  defaultPalette =
    if preferredPalette == null then
      catalogue.${defaultHost}.herdr.name
    else if builtins.hasAttr preferredPalette catalogue then
      catalogue.${preferredPalette}.herdr.name
    else if builtins.any (theme: theme.herdr.name == preferredPalette) (lib.attrValues catalogue) then
      preferredPalette
    else
      throw "Unknown Hyprland appearance.palette: ${preferredPalette}";
  paletteOverrides =
    mode:
    let
      overrides = configuredAppearance.paletteOverrides.${mode} or { };
      valid = lib.all (
        key:
        builtins.hasAttr key catalogue.${defaultHost}.${mode}
        && builtins.isString overrides.${key}
        && builtins.match "[0-9a-fA-F]{6}" overrides.${key} != null
      ) (lib.attrNames overrides);
    in
    assert lib.assertMsg valid
      "Hyprland paletteOverrides must contain Base16 keys and six-digit hex colours without #";
    overrides;
  themedCatalogue = lib.mapAttrs (
    _: theme:
    theme
    // {
      light = theme.light // paletteOverrides "light";
      dark = theme.dark // paletteOverrides "dark";
    }
  ) catalogue;
  entries = lib.mapAttrs (
    host: theme:
    let
      cosmic = import ./cosmic.nix { inherit pkgs theme; };
      herdr = (pkgs.formats.toml { }).generate "${host}-herdr.toml" (
        lib.recursiveUpdate (builtins.fromTOML (builtins.readFile ../../config/herdr/config.toml)) {
          theme = theme.herdr // {
            custom = lib.genAttrs [ "light" "dark" ] (mode: render.herdr theme.${mode});
          };
        }
      );
      nvim = lib.genAttrs [ "light" "dark" ] (mode: lib.mapAttrs (_: c: "#${c}") theme.${mode});
    in
    {
      inherit (theme) label;
      nativeMode = theme.nativeMode or null;
      id = theme.herdr.name;
      inherit nvim;
      herdrTheme = theme.herdr // {
        custom = lib.genAttrs [ "light" "dark" ] (mode: render.herdr theme.${mode});
      };
      cosmic = toString cosmic;
      session = lib.genAttrs [ "light" "dark" ] (
        mode:
        lib.mapAttrs (_: text: toString (pkgs.writeText "${host}-${mode}-hyprland-theme" text)) (
          hyprland.render {
            inherit mode;
            palette = theme.${mode};
            appearance = configuredAppearance;
          }
        )
        // {
          "wallpaper.png" = toString (
            wallpaperSettings.overrides.${theme.herdr.name}.${mode} or omarchy.wallpapers.${theme.herdr.name}
          );
        }
      );
      files = {
        "herdr.toml" = toString herdr;
        "host-palettes.json" = toString (pkgs.writeText "${host}-nvim.json" (builtins.toJSON nvim));
      }
      // lib.listToAttrs (
        lib.concatMap
          (mode: [
            {
              name = "ghostty-${mode}";
              value = toString (pkgs.writeText "${host}-ghostty-${mode}" (render.ghostty theme.${mode}));
            }
            {
              name = "host-${mode}.json";
              value = toString (
                pkgs.writeText "${host}-pi-${mode}.json" (builtins.toJSON (render.pi "host-${mode}" theme.${mode}))
              );
            }
          ])
          [
            "light"
            "dark"
          ]
      );
    }
  ) themedCatalogue;
  manifest = pkgs.writeText "theme-catalogue.json" (
    builtins.toJSON {
      default = defaultPalette;
      appearance = configuredAppearance;
      inherit (import ../../config/hyprland/settings.nix) launcher;
      palettes = lib.mapAttrs' (_: entry: lib.nameValuePair entry.id entry) entries;
    }
  );
  schemaData = "${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.name}";
  dconfModules = "${pkgs.dconf.lib}/lib/gio/modules";
  package = pkgs.writeShellApplication {
    name = "theme-menu";
    runtimeInputs = [
      pkgs.python3
      pkgs.fuzzel
      pkgs.glib
      pkgs.dconf
      pkgs.systemd
      pkgs.mako
      pkgs.herdr
      pkgs.libnotify
    ];
    text = ''
      export THEME_MENU_PUBLISH=1
      # Schemas live under share/gsettings-schemas/<name>, not share/.
      # dconf.lib supplies the GIO backend; dconf is the user database tool.
      export XDG_DATA_DIRS="${schemaData}:''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
      export GIO_EXTRA_MODULES="${dconfModules}''${GIO_EXTRA_MODULES:+:}''${GIO_EXTRA_MODULES:-}"
      exec python3 ${../../config/appearance/theme_menu.py} --catalogue ${manifest} --state ${lib.escapeShellArg state} "$@"
    '';
  };
in
{
  inherit state manifest package;
  inherit (omarchy) license;
  file = name: config.lib.file.mkOutOfStoreSymlink "${state}/active/${name}";
}
