{
  config,
  lib,
  pkgs,
  catalogue,
  defaultHost,
}:
let
  state = "${config.xdg.stateHome}/theme-menu";
  controller = import ../../../programs/theme-menu { inherit pkgs; };
  render = import ./render.nix { inherit lib; };
  herdrTheme = import ./herdr.nix { };
  btopTheme = import ./btop.nix { inherit lib; };
  hyprland = import ./hyprland.nix { inherit lib; };
  omarchy = import ./omarchy.nix { inherit lib; };
  # Rendered theme text is content-addressed. A nixpkgs bump must not rebuild it.
  textFile = name: text: builtins.toFile name text;
  tomlString =
    value: if builtins.isBool value then (if value then "true" else "false") else builtins.toJSON value;
  tomlPairs =
    attrs:
    lib.concatMapStringsSep "\n" (name: "${name} = ${tomlString attrs.${name}}") (
      lib.sort builtins.lessThan (lib.attrNames attrs)
    );
  herdrToml =
    config:
    let
      flat = lib.filterAttrs (name: _: name != "custom") config;
      custom = mode: ''
        [theme.custom.${mode}]
        ${tomlPairs config.custom.${mode}}
      '';
    in
    ''
      [theme]
      ${tomlPairs flat}

      ${custom "dark"}
      ${custom "light"}
    '';
  spliceTheme =
    text: replacement:
    let
      lines = lib.splitString "\n" text;
      start = lib.lists.findFirstIndex (line: line == "[theme]") null lines;
      rest = if start == null then [ ] else lib.sublist (start + 1) (builtins.length lines) lines;
      end =
        if start == null then
          null
        else
          lib.lists.findFirstIndex (line: lib.hasPrefix "[" line && !lib.hasPrefix "[theme" line) null rest;
    in
    assert lib.assertMsg (start != null && end != null) "herdr config is missing a [theme] section";
    lib.concatStringsSep "\n" (
      lib.sublist 0 start lines
      ++ lib.splitString "\n" (lib.removeSuffix "\n" replacement)
      ++ lib.sublist (start + 1 + end) (builtins.length lines) lines
    );
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
      herdrConfig = herdrTheme.configTheme theme (
        lib.genAttrs [ "light" "dark" ] (mode: render.herdr theme.${mode})
      );
      herdr = textFile "${host}-herdr.toml" (
        spliceTheme (builtins.readFile ../../config/herdr/config.toml) (herdrToml herdrConfig)
      );
      nvim = lib.genAttrs [ "light" "dark" ] (mode: lib.mapAttrs (_: c: "#${c}") theme.${mode});
      artwork = omarchy.selection theme;
    in
    {
      inherit (theme) label;
      nativeMode = theme.nativeMode or null;
      id = theme.herdr.name;
      inherit nvim;
      inherit (artwork) backgrounds preferred wallpaperColor;
      herdrTheme = herdrConfig;
      session = lib.genAttrs [ "light" "dark" ] (
        mode:
        let
          rendered = hyprland.render {
            inherit mode;
            palette = theme.${mode};
            appearance = configuredAppearance;
          };
        in
        # Seed and publish every desktop asset, including walker.css. Wallpaper
        # stays beside them; it is not a generated text asset.
        assert lib.assertMsg (
          lib.subtractLists (builtins.attrNames rendered) hyprland.assets == [ ]
        ) "Hyprland theme render is missing a seed asset";
        lib.mapAttrs (name: text: textFile "${host}-${mode}-${name}" text) rendered
        // {
          # A store path would root every theme image in the Home Manager
          # generation. theme-menu materialises the locked theme fetch on apply.
          "wallpaper.png" = wallpaperSettings.overrides.${theme.herdr.name}.${mode} or "nix-theme:${host}";
        }
      );
      files = {
        "herdr.toml" = herdr;
        "host-palettes.json" = textFile "${host}-nvim.json" (builtins.toJSON nvim);
        "btop.theme" = textFile "btop-${host}.theme" (
          btopTheme theme.herdr.name theme.${theme.nativeMode or "dark"}
        );
        "delta" = textFile "${host}-delta" (render.deltaFragment theme);
        "bat-${theme.nativeMode or "dark"}" = textFile "${host}-bat-native.tmTheme" (
          render.tmTheme "host-${theme.nativeMode or "dark"}" theme.${theme.nativeMode or "dark"}
        );
      }
      // lib.listToAttrs (
        lib.concatMap
          (mode: [
            {
              name = "ghostty-${mode}";
              value = textFile "${host}-ghostty-${mode}" (render.ghostty theme.${mode});
            }
            {
              name = "host-${mode}.json";
              value = textFile "${host}-pi-${mode}.json" (
                builtins.toJSON (render.pi "host-${mode}" theme.${mode})
              );
            }
            {
              # bat/delta reads the active syntax theme from BAT_THEME. Ships
              # both modes so light/dark swaps never need a rebuild.
              name = "bat-${mode}";
              value = textFile "${host}-bat-${mode}.tmTheme" (render.tmTheme "host-${mode}" theme.${mode});
            }
          ])
          [
            "light"
            "dark"
          ]
      );
    }
  ) themedCatalogue;
  manifest = textFile "theme-catalogue.json" (
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
      pkgs.fuzzel
      pkgs.glib
      pkgs.dconf
      pkgs.systemd
      pkgs.mako
      pkgs.herdr
      pkgs.libnotify
      pkgs.imagemagick
    ];
    text = ''
      export THEME_MENU_PUBLISH=1
      export THEME_MAGICK=${lib.escapeShellArg "${pkgs.imagemagick}/bin/magick"}
      export THEME_WALLPAPER_FLAKE=${lib.escapeShellArg config.dotfiles.sourceDirectory}
      # Schemas live under share/gsettings-schemas/<name>, not share/.
      # dconf.lib supplies the GIO backend; dconf is the user database tool.
      export XDG_DATA_DIRS="${schemaData}:''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
      export GIO_EXTRA_MODULES="${dconfModules}''${GIO_EXTRA_MODULES:+:}''${GIO_EXTRA_MODULES:-}"
      exec ${controller}/bin/theme-menu --catalogue ${manifest} --state ${lib.escapeShellArg state} "$@"
    '';
  };
in
{
  inherit state manifest package;
  inherit (omarchy) license;
  file = name: config.lib.file.mkOutOfStoreSymlink "${state}/active/${name}";
}
