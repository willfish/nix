{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "audiobook-library";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/command-support
      ../lib/repo-data
      ../pi-config/src/json.c
      ../pi-config/include/json.h
    ];
  };
  sourceRoot = "source/audiobook-library";
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
    description = "Inspect audiobook source inventories and check destination duplicates";
    mainProgram = "qbittorrent-inventory";
    platforms = pkgs.lib.platforms.unix;
  };
}
