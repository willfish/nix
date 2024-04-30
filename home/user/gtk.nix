{ pkgs, pkgs-unstable, ... }:
{
  gtk = {
    enable = true;

    cursorTheme = {
        name = "Catppuccin-Macchiato-Blue";
        package = pkgs-unstable.catppuccin-cursors.macchiatoBlue;
    };

    theme = {
      name = "Catppuccin-Macchiato-Compact-Blue-dark";
      package = pkgs-unstable.catppuccin-gtk.override {
        size = "compact";
        accents = ["blue"];
        variant = "macchiato";
      };
    };

    iconTheme = {
      name = "Papirus-Dark";
      package = pkgs-unstable.papirus-folders;
    };

    gtk3.extraConfig = {
      Settings = ''
            gtk-application-prefer-dark-theme = 1;
      '';
    };

    gtk4.extraConfig = {
      Settings = ''
            gtk-application-prefer-dark-theme = 1;
      '';
    };

  };
}
