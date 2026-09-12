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
  names = {
    andromeda = "Rosé Pine";
    foundation = "Tokyo Night";
    starfish = "Solarized";
    terminus = "Catppuccin";
    relay = "Gruvbox";
  };
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
      label = names.${host};
      id = theme.herdr.name;
      inherit nvim;
      herdrTheme = theme.herdr // {
        custom = lib.genAttrs [ "light" "dark" ] (mode: render.herdr theme.${mode});
      };
      cosmic = toString cosmic;
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
  ) catalogue;
  manifest = pkgs.writeText "theme-catalogue.json" (
    builtins.toJSON {
      default = catalogue.${defaultHost}.herdr.name;
      palettes = lib.mapAttrs' (_: entry: lib.nameValuePair entry.id entry) entries;
    }
  );
  package = pkgs.writeShellApplication {
    name = "theme-menu";
    runtimeInputs = [
      pkgs.python3
      pkgs.fuzzel
      pkgs.glib
      pkgs.herdr
      pkgs.libnotify
    ];
    text = ''
      exec python3 ${../../config/appearance/theme_menu.py} --catalogue ${manifest} --state ${lib.escapeShellArg state} "$@"
    '';
  };
in
{
  inherit state manifest package;
  file = name: config.lib.file.mkOutOfStoreSymlink "${state}/active/${name}";
}
