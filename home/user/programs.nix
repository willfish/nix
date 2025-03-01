{ pkgs-unstable, ... }:
{
  programs = {
    brave = {
      enable = true;

      dictionaries = [
        pkgs-unstable.hunspellDictsChromium.en_GB
      ];
    };

    google-chrome = {
      enable = true;
    };

    direnv = {
      enable = true;
      nix-direnv.enable = true;
    };
  };
}
