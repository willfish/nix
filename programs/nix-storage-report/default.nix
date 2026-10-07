{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "nix-storage-report";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/repo-data
    ];
  };
  sourceRoot = "source/nix-storage-report";
  nativeBuildInputs = [
    pkgs.meson
    pkgs.ninja
    pkgs.pkg-config
  ];
  buildInputs = [
    pkgs.glib
    pkgs.yyjson
  ];
  doCheck = false;
  meta = {
    description = "Report direct, shared and unique Nix store closure sizes";
    mainProgram = "nix-storage-report";
    platforms = pkgs.lib.platforms.unix;
  };
}
