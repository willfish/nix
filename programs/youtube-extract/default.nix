{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "youtube-extract";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/command-support
      ../lib/repo-data
    ];
  };
  sourceRoot = "source/youtube-extract";
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
    description = "Extract YouTube metadata and timestamped subtitle text without media downloads";
    mainProgram = "youtube-extract";
    platforms = pkgs.lib.platforms.unix;
  };
}
