{
  pkgs,
  hostTheme,
  ...
}:
{
  stylix = {
    enable = true;
    autoEnable = true;
    polarity = "dark";
    base16Scheme = hostTheme.dark;

    fonts = {
      monospace = {
        package = pkgs.nerd-fonts.jetbrains-mono;
        name = "JetBrainsMono Nerd Font";
      };
      sansSerif = {
        package = pkgs.ubuntu-classic;
        name = "Ubuntu";
      };
      serif = {
        package = pkgs.ubuntu-classic;
        name = "Ubuntu";
      };
    };

    targets.gnome.enable = false;

    # Paired, runtime-switchable targets are owned by appearance.nix.
  };

  programs.nix-index = {
    enable = true;
    enableFishIntegration = true;
  };

  programs.nix-index-database.comma.enable = true;
}
