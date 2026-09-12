{
  config,
  lib,
  pkgs,
  hostTheme,
  ...
}:
{
  stylix = {
    enable = true;
    # Only theme software this home actually uses. autoEnable would also
    # configure GNOME, Hyprland, Firefox, and other apps that are not installed,
    # and unused GTK/GNOME targets write dconf that breaks headless activation.
    autoEnable = false;
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

    targets.font-packages.enable = true;
    targets.fontconfig.enable = true;

    # Paired, runtime-switchable targets are owned by appearance.nix.
  };

  dconf.enable = lib.mkIf (!config.dotfiles.capabilities.desktop) false;

  programs.nix-index = {
    enable = true;
    enableFishIntegration = true;
  };

  programs.nix-index-database.comma.enable = true;
}
