{
  pkgs,
  ...
}:
{
  stylix = {
    enable = true;
    autoEnable = true;
    polarity = "dark";
    base16Scheme = "${pkgs.base16-schemes}/share/themes/rose-pine.yaml";

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

    targets.neovim = {
      enable = true;
      transparentBackground.main = true;
      transparentBackground.signColumn = true;
    };
  };

  programs.nix-index = {
    enable = true;
    enableFishIntegration = true;
  };

  programs.nix-index-database.comma.enable = true;
}
