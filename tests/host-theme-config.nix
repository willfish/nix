# Read-only contract query used by test_host_theme_config.py.
let
  flake = builtins.getFlake (toString ../.);
  settings = import ../home/config/hyprland/settings.nix;
  palettes = import ../home/user/themes/palettes.nix;
  expectedThemes = {
    "william@andromeda" = "rose-pine";
    "william@foundation" = "tokyo-night";
    "william@starfish" = "osaka-jade";
    "william@terminus" = "catppuccin";
    "william@relay" = "gruvbox";
    "william-darwin" = "gruvbox";
    "william-linux" = "rose-pine";
  };
  textOf = file: if file == null then null else file.source.text or file.text or null;
  inspect =
    configuration:
    let
      home = flake.homeConfigurations.${configuration};
      c = home.config;
      files = c.home.file;
      runtime = c.xdg.configFile ? "theme-menu/catalogue.json";
      catalogue =
        if runtime then
          builtins.fromJSON (
            builtins.unsafeDiscardStringContext c.xdg.configFile."theme-menu/catalogue.json".source.text
          )
        else
          null;
      selected = if runtime then catalogue.palettes.${catalogue.default} else null;
      herdr =
        if runtime then
          { theme = selected.herdrTheme; }
        else
          builtins.fromJSON files.".config/herdr/config.toml".source.value;
      expectedTheme = expectedThemes.${configuration};
      fuzzel = c.xdg.configFile."fuzzel/fuzzel.ini" or null;
      hyprlandFuzzel = c.xdg.configFile."fuzzel/hyprland.ini" or null;
      voiceFuzzel = c.xdg.configFile."voice-menu/fuzzel.ini" or null;
    in
    {
      inherit (herdr) theme;
      stylix = c.lib.stylix.colors.base00;
      ghostty = if files ? ".config/ghostty" then toString files.".config/ghostty".source else null;
      pi = files.".local/bin/pi".text;
      nvim =
        if runtime then selected.nvim else builtins.fromJSON files.".config/nvim/host-palettes.json".text;
      graphical = runtime;
      inherit catalogue;
      reapply = c.home.activation.applySelectedPalette.data or "";
      gtkFixed = c.stylix.targets.gtk.enable;
      gtkEnable = c.gtk.enable;
      dconfSettings = builtins.attrNames c.dconf.settings;
      stylixAutoEnable = c.stylix.autoEnable;
      fishFixed = c.stylix.targets.fish.enable;
      delta = c.programs.git.settings.delta or { };
      activeFuzzel = "${c.xdg.stateHome}/theme-menu/active/fuzzel.ini";
      fuzzelIni = textOf fuzzel;
      hyprlandFuzzel = textOf hyprlandFuzzel;
      voiceFuzzel = textOf voiceFuzzel;
      sameDefaultFuzzel = fuzzel != null && fuzzel.source == hyprlandFuzzel.source;
      voiceMenu = settings.menus.voice;
      inherit expectedTheme;
      themeMatchesHost = c.lib.stylix.colors.base00 == palettes.${expectedTheme}.dark.base00;
      paletteNames = builtins.attrNames palettes;
    };
in
builtins.listToAttrs (
  map (configuration: {
    name = configuration;
    value = inspect configuration;
  }) (builtins.attrNames expectedThemes)
)
