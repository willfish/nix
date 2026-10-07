{ pkgs }:
pkgs.symlinkJoin {
  name = "skill-tools-0.1.0";
  paths = [
    (import ../../color-contrast { inherit pkgs; })
    (import ../../youtube-extract { inherit pkgs; })
    (import ../../audiobook-library { inherit pkgs; })
    (import ../../pi-token-report { inherit pkgs; })
  ];
  meta = {
    description = "Compatibility collection of native skill commands";
    platforms = pkgs.lib.platforms.unix;
  };
}
