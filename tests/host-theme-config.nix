# Read-only contract query used by test_host_theme_config.py.
let
  flake = builtins.getFlake (toString ../.);
  inspect =
    configuration:
    let
      home = flake.homeConfigurations.${configuration};
      c = home.config;
      files = c.home.file;
      herdr = builtins.fromJSON files.".config/herdr/config.toml".source.value;
    in
    {
      inherit (herdr) theme;
      stylix = c.lib.stylix.colors.base00;
      ghostty = toString files.".config/ghostty".source;
      pi = files.".local/bin/pi".text;
      nvim = builtins.fromJSON files.".config/nvim/host-palettes.json".text;
      cosmic = c.xdg.configFile ? "cosmic/com.system76.CosmicTheme.Dark/v1";
      writableMode = !(c.xdg.configFile ? "cosmic/com.system76.CosmicTheme.Mode/v1/is_dark");
      gtkFixed = c.stylix.targets.gtk.enable;
      fishFixed = c.stylix.targets.fish.enable;
      tmuxFixed = c.stylix.targets.tmux.enable;
      cosmicSource =
        if c.xdg.configFile ? "cosmic/com.system76.CosmicTheme.Dark/v1" then
          toString c.xdg.configFile."cosmic/com.system76.CosmicTheme.Dark/v1".source
        else
          null;
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
