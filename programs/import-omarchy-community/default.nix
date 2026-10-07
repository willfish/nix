{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "import-omarchy-community";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/repo-data
    ];
  };
  sourceRoot = "source/import-omarchy-community";
  nativeBuildInputs = [
    pkgs.meson
    pkgs.ninja
    pkgs.pkg-config
  ];
  buildInputs = [
    pkgs.glib
    pkgs.yyjson
    pkgs.libxml2
  ];
  doCheck = false;
  meta = {
    description = "Import reviewed Omarchy community-theme catalogue data";
    mainProgram = "import-omarchy-community";
    platforms = pkgs.lib.platforms.unix;
  };
}
