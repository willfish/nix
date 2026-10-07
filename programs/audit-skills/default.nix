{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "audit-skills";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../lib/repo-data
    ];
  };
  sourceRoot = "source/audit-skills";
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
    description = "Audit the public skill catalogue and deployment references";
    mainProgram = "audit-skills";
    platforms = pkgs.lib.platforms.unix;
  };
}
