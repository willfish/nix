{ pkgs }:
pkgs.symlinkJoin {
  name = "repo-tools-0.1.0";
  paths = [
    (import ../../check-flake-lock-update { inherit pkgs; })
    (import ../../import-omarchy-community { inherit pkgs; })
    (import ../../audit-skills { inherit pkgs; })
    (import ../../nix-storage-report { inherit pkgs; })
  ];
  meta = {
    description = "Compatibility collection of repository data commands";
    mainProgram = "check-flake-lock-update";
    platforms = pkgs.lib.platforms.unix;
  };
}
