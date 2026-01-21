{ pkgs, ... }:
{
  programs = {
    brave = {
      enable = true;

      dictionaries = [
        pkgs.hunspellDictsChromium.en_GB
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
