{
  config,
  lib,
  pkgs,
  hostTheme,
  isGraphicalLinux,
  ...
}:
let
  inherit (import ../config/hyprland/settings.nix) appearance;
  omarchyFonts = {
    inherit (appearance) font serifFont monoFont;
    emoji = "Noto Color Emoji";
    arabic = "Noto Naskh Arabic";
    urdu = "Noto Nastaliq Urdu";
  };
  omarchyFontconfig = import ../config/fontconfig/omarchy-conf.nix omarchyFonts;
in
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
        name = omarchyFonts.monoFont;
      };
      sansSerif = {
        package = pkgs.liberation_ttf;
        name = omarchyFonts.font;
      };
      serif = {
        package = pkgs.liberation_ttf;
        name = omarchyFonts.serifFont;
      };
      emoji = {
        package = pkgs.noto-fonts-color-emoji;
        name = omarchyFonts.emoji;
      };
    };

    fonts.sizes.applications = lib.mkIf isGraphicalLinux appearance.fontSize;

    targets.font-packages.enable = true;
    targets.fontconfig.enable = true;

    # Paired, runtime-switchable targets are owned by appearance.nix.
  };

  # Coverage fonts are not stylix roles. Noto Sans includes Naskh and Nastaliq.
  home.packages = lib.optionals isGraphicalLinux [
    pkgs.noto-fonts
    pkgs.noto-fonts-cjk-sans
  ];

  fonts.fontconfig = {
    enable = true;
    configFile.omarchy = {
      enable = true;
      label = "omarchy";
      priority = 90;
      text = omarchyFontconfig;
    };
  };

  dconf.enable = lib.mkIf (!config.dotfiles.capabilities.desktop) false;

  programs.nix-index = {
    enable = true;
    enableFishIntegration = true;
  };

  programs.nix-index-database.comma.enable = true;
}
