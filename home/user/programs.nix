{ pkgs, ... }:
{
  programs = {
    brave = {
      enable = true;

      commandLineArgs = [
        "--remote-debugging-port=9222"
      ];

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
