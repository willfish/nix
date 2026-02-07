{ pkgs, lib, ... }:
let
  inherit (pkgs) stdenv;
in
{
  programs = {
    brave = lib.mkIf stdenv.isLinux {
      enable = true;

      commandLineArgs = [
        "--remote-debugging-port=9222"
      ];

      dictionaries = [
        pkgs.hunspellDictsChromium.en_GB
      ];
    };

    google-chrome = lib.mkIf stdenv.isLinux {
      enable = true;
    };

    direnv = {
      enable = true;
      nix-direnv.enable = true;
    };

    atuin = lib.mkIf stdenv.isLinux {
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
