{
  config,
  lib,
  pkgs,
  hostName,
  isGraphicalLinux,
  ...
}:
let
  catalogue = import ./themes/palettes.nix;
  desktopAppearance = (import ../config/hyprland/settings.nix).appearance;
  ghosttyConfig = builtins.readFile ../config/ghostty/config;
  defaultHost =
    if hostName != null && builtins.hasAttr hostName catalogue then hostName else "andromeda";
  theme = catalogue.${defaultHost};
  runtime = import ./themes/runtime.nix {
    inherit
      config
      lib
      pkgs
      catalogue
      defaultHost
      ;
  };
  render = import ./themes/render.nix { inherit lib; };
  piThemes = lib.genAttrs [ "light" "dark" ] (
    mode: pkgs.writeText "host-${mode}.json" (builtins.toJSON (render.pi "host-${mode}" theme.${mode}))
  );
  piFile = mode: if isGraphicalLinux then runtime.file "host-${mode}.json" else piThemes.${mode};
in
{
  _module.args.hostTheme = theme;
  _module.args.herdrThemeFile = if isGraphicalLinux then runtime.file "herdr.toml" else null;
  _module.args.piThemeArgs =
    if isGraphicalLinux then
      "--theme ${runtime.state}/active/host-light.json --theme ${runtime.state}/active/host-dark.json --use-theme host-light/host-dark"
    else
      "--theme ${piThemes.light} --theme ${piThemes.dark} --use-theme host-light/host-dark";

  home.packages = lib.optionals isGraphicalLinux [ runtime.package ];

  # These applications have native runtime light/dark selection. Do not let a
  # second theming module replace their paired configuration with a fixed theme.
  stylix.targets.ghostty.enable = false;
  stylix.targets.neovim.enable = false;
  stylix.targets.fish.enable = false;
  stylix.targets.gtk.enable = false;
  stylix.targets.qt.enable = false;
  stylix.targets.kde.enable = false;

  programs.fish.interactiveShellInit = lib.mkAfter (
    builtins.readFile ../config/fish/host-colours.fish
  );

  home.file = {
    # Keep ownership of the existing directory symlink for a clean HM migration.
    ".config/ghostty" = lib.mkIf (!pkgs.stdenv.isDarwin || config.dotfiles.capabilities.localTerminal) {
      source = pkgs.linkFarm "host-ghostty" [
        {
          name = "config";
          path = pkgs.writeText "ghostty-config" (
            (
              if isGraphicalLinux then
                builtins.replaceStrings
                  [ ''font-family = "JetBrainsMono Nerd Font"'' "font-size = 12" ]
                  [
                    ''font-family = "${desktopAppearance.monoFont}"''
                    "font-size = ${toString desktopAppearance.fontSize}"
                  ]
                  ghosttyConfig
              else
                ghosttyConfig
            )
            + ''
              theme = light:host-light,dark:host-dark
            ''
          );
        }
        {
          name = "themes/host-dark";
          path =
            if isGraphicalLinux then
              runtime.file "ghostty-dark"
            else
              pkgs.writeText "ghostty-dark" (render.ghostty theme.dark);
        }
        {
          name = "themes/host-light";
          path =
            if isGraphicalLinux then
              runtime.file "ghostty-light"
            else
              pkgs.writeText "ghostty-light" (render.ghostty theme.light);
        }
      ];
    };
    ".pi/agent/themes/host-dark.json".source = piFile "dark";
    ".pi/agent/themes/host-light.json".source = piFile "light";
    ".config/nvim/host-palettes.json" =
      if isGraphicalLinux then
        {
          source = runtime.file "host-palettes.json";
        }
      else
        {
          text = builtins.toJSON {
            dark = lib.mapAttrs (_: c: "#${c}") theme.dark;
            light = lib.mapAttrs (_: c: "#${c}") theme.light;
          };
        };
    ".config/nvim/lua/host-theme.lua".source = ../config/nvim/host-theme.lua;
  };

  xdg.configFile = lib.mkIf isGraphicalLinux {
    "theme-menu/catalogue.json".source = runtime.manifest;
    # COSMIC exports both variants and applies GTK/Qt when the mode changes.
    "cosmic/com.system76.CosmicTk/v1/apply_theme_global".text = "true";
  };

  gtk = lib.mkIf isGraphicalLinux {
    enable = true;
    font = {
      inherit (config.stylix.fonts.sansSerif) package name;
      size = config.stylix.fonts.sizes.applications;
    };
    theme = {
      package = pkgs.adw-gtk3;
      name = "adw-gtk3";
    };
    gtk4.theme = null;
  };

  home.activation = {
    applySelectedPalette = lib.mkIf isGraphicalLinux (
      lib.hm.dag.entryAfter [ "linkGeneration" "writableCosmicMode" ] ''
        run ${runtime.package}/bin/theme-menu --reapply
      ''
    );
    rememberCosmicMode = lib.mkIf isGraphicalLinux (
      lib.hm.dag.entryBetween [ "linkGeneration" ] [ "writeBoundary" ] ''
        cosmicModePath="$HOME/.config/cosmic/com.system76.CosmicTheme.Mode/v1/is_dark"
        savedCosmicMode=true
        if [ -f "$cosmicModePath" ] && grep -qx false "$cosmicModePath"; then
          savedCosmicMode=false
        fi
      ''
    );
    writableCosmicMode = lib.mkIf isGraphicalLinux (
      lib.hm.dag.entryAfter [ "linkGeneration" ] ''
        modeDir="$HOME/.config/cosmic/com.system76.CosmicTheme.Mode/v1"
        mkdir -p "$modeDir"
        if [ -L "$modeDir/is_dark" ] || [ ! -e "$modeDir/is_dark" ]; then
          printf '%s\n' "$savedCosmicMode" > "$modeDir/.is_dark.new"
          mv -f "$modeDir/.is_dark.new" "$modeDir/is_dark"
        fi
        # No sunrise/sunset scheduling. Leave the actual light/dark choice writable.
        printf 'false\n' > "$modeDir/.auto_switch.new"
        mv -f "$modeDir/.auto_switch.new" "$modeDir/auto_switch"
      ''
    );
  };
}
