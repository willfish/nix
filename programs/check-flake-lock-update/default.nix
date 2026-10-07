{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "check-flake-lock-update";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/repo-data
    ];
  };
  sourceRoot = "source/check-flake-lock-update";
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
    description = "Validate flake lock changes against the repository update policy";
    mainProgram = "check-flake-lock-update";
    platforms = pkgs.lib.platforms.unix;
  };
}
