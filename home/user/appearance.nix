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
  theme =
    catalogue.${
      if hostName != null && builtins.hasAttr hostName catalogue then hostName else "andromeda"
    };
  render = import ./themes/render.nix { inherit lib; };
  cosmic = import ./themes/cosmic.nix { inherit pkgs theme; };
  piThemes = lib.genAttrs [ "light" "dark" ] (
    mode: pkgs.writeText "host-${mode}.json" (builtins.toJSON (render.pi "host-${mode}" theme.${mode}))
  );
  cosmicFiles = lib.listToAttrs (
    map
      (name: {
        name = "cosmic/com.system76.CosmicTheme.${name}/v1";
        value = {
          source = "${cosmic}/cosmic/com.system76.CosmicTheme.${name}/v1";
          recursive = true;
          force = true;
        };
      })
      [
        "Dark"
        "Light"
        "Dark.Builder"
        "Light.Builder"
      ]
  );
in
{
  _module.args.hostTheme = theme;
  _module.args.piThemeArgs = "--theme ${piThemes.light} --theme ${piThemes.dark} --use-theme host-light/host-dark";

  # These applications have native runtime light/dark selection. Do not let a
  # second theming module replace their paired configuration with a fixed theme.
  stylix.targets.ghostty.enable = false;
  stylix.targets.neovim.enable = false;
  stylix.targets.fish.enable = false;
  stylix.targets.tmux.enable = false;
  stylix.targets.gtk.enable = lib.mkIf isGraphicalLinux false;
  stylix.targets.qt.enable = lib.mkIf isGraphicalLinux false;
  stylix.targets.kde.enable = lib.mkIf isGraphicalLinux false;

  programs.fish.interactiveShellInit = lib.mkAfter (
    builtins.readFile ../config/fish/host-colours.fish
  );

  home.file = {
    # Keep ownership of the existing directory symlink for a clean HM migration.
    ".config/ghostty".source = pkgs.linkFarm "host-ghostty" [
      {
        name = "config";
        path = pkgs.writeText "ghostty-config" (
          builtins.readFile ../config/ghostty/config
          + ''
            theme = light:host-light,dark:host-dark
          ''
        );
      }
      {
        name = "themes/host-dark";
        path = pkgs.writeText "ghostty-dark" (render.ghostty theme.dark);
      }
      {
        name = "themes/host-light";
        path = pkgs.writeText "ghostty-light" (render.ghostty theme.light);
      }
    ];
    ".pi/agent/themes/host-dark.json".source = piThemes.dark;
    ".pi/agent/themes/host-light.json".source = piThemes.light;
    ".config/nvim/host-palettes.json".text = builtins.toJSON {
      dark = lib.mapAttrs (_: c: "#${c}") theme.dark;
      light = lib.mapAttrs (_: c: "#${c}") theme.light;
    };
    ".config/nvim/lua/host-theme.lua".source = ../config/nvim/host-theme.lua;
  };

  xdg.configFile = lib.mkIf isGraphicalLinux (
    cosmicFiles
    // {
      # COSMIC exports both variants and applies GTK/Qt when the mode changes.
      "cosmic/com.system76.CosmicTk/v1/apply_theme_global".text = "true";
    }
  );

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
    piEditorPadding = lib.hm.dag.entryAfter [ "linkGeneration" ] ''
      ${pkgs.python3}/bin/python3 - <<'PY'
      import json, os
      from pathlib import Path
      path = Path.home() / ".pi/agent/settings.json"
      settings = json.loads(path.read_text()) if path.exists() else {}
      if "editorPaddingX" not in settings:
          settings["editorPaddingX"] = 1
          temporary = path.with_suffix(".json.tmp")
          temporary.write_text(json.dumps(settings, indent=2) + "\n")
          temporary.chmod(0o600)
          os.replace(temporary, path)
      PY
    '';
  };
}
