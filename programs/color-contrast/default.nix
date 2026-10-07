{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "color-contrast";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/command-support
      ../lib/repo-data
    ];
  };
  sourceRoot = "source/color-contrast";
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
    description = "Calculate WCAG contrast ratios for opaque sRGB colors";
    mainProgram = "contrast";
    platforms = pkgs.lib.platforms.unix;
  };
}
