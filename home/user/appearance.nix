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
  defaultHost = (import ./themes/host-defaults.nix).forHost hostName;
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
  btopTheme = import ./themes/btop.nix { inherit lib pkgs; };
  btopFile = btopTheme theme.herdr.name theme.${theme.nativeMode or "dark"};
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

  programs.btop = {
    enable = true;
    settings = {
      color_theme = "host";
      theme_background = true;
      truecolor = true;
      # The config is managed. Saving on exit would rewrite the Nix file and
      # can drop the selected theme name.
      save_config_on_exit = false;
    };
  };

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

  xdg.configFile = {
    "btop/btop.conf".force = true;
    "btop/themes/host.theme" = {
      force = true;
      source = if isGraphicalLinux then runtime.file "btop.theme" else btopFile;
    };
  }
  // lib.optionalAttrs isGraphicalLinux {
    "theme-menu/catalogue.json".source = runtime.manifest;
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
      lib.hm.dag.entryAfter [ "linkGeneration" ] ''
        run ${runtime.package}/bin/theme-menu --reapply
      ''
    );
  };
}
