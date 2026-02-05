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

    atuin = {
      enable = true;
      enableFishIntegration = true;
      enableBashIntegration = true;
      settings = {
        style = "compact";
        inline_height = 20;
      };
    };
  };
}
