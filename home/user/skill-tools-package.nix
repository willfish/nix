{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "skill-tools";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../config;
    fileset = pkgs.lib.fileset.unions [
      ../config/skill-tools
      ../config/repo-tools/common.c
      ../config/repo-tools/common.h
    ];
  };
  sourceRoot = "source/skill-tools";
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = with pkgs; [
    glib
    yyjson
  ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
