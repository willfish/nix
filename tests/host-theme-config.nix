# Read-only contract query used by test_host_theme_config.py.
let
  flake = builtins.getFlake (toString ../.);
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
    in
    {
      inherit (herdr) theme;
      stylix = c.lib.stylix.colors.base00;
      ghostty = if files ? ".config/ghostty" then toString files.".config/ghostty".source else null;
      pi = files.".local/bin/pi".text;
      nvim =
        if runtime then selected.nvim else builtins.fromJSON files.".config/nvim/host-palettes.json".text;
      cosmic = runtime;
      inherit catalogue;
      reapply = c.home.activation.applySelectedPalette.data or "";
      writableMode = !(c.xdg.configFile ? "cosmic/com.system76.CosmicTheme.Mode/v1/is_dark");
      gtkFixed = c.stylix.targets.gtk.enable;
      gtkEnable = c.gtk.enable;
      dconfSettings = builtins.attrNames c.dconf.settings;
      stylixAutoEnable = c.stylix.autoEnable;
      fishFixed = c.stylix.targets.fish.enable;
      tmuxFixed = c.stylix.targets.tmux.enable;
      cosmicSource = if runtime then selected.cosmic else null;
      rememberMode = c.home.activation.rememberCosmicMode.data or "";
      writableModeScript = c.home.activation.writableCosmicMode.data or "";
    };
in
builtins.listToAttrs (
  map
    (configuration: {
      name = configuration;
      value = inspect configuration;
    })
    [
      "william@andromeda"
      "william@foundation"
      "william@starfish"
      "william@terminus"
      "william-darwin"
    ]
)
