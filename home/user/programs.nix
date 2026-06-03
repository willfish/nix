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

    # Disable man-db cache generation during activation. The openssl package
    # ships ~6200 man3 pages (including a cluster of OSSL_CMP_* ones that
    # mandb 2.13.1 frequently fails to index). This produces noisy but harmless
    # "failed to store entry" warnings in the man-cache step of `nh home switch`.
    # Direct `man` still works; we just skip the (slow, spammy) whatis indexing.
    man.generateCaches = false;
  };
}
