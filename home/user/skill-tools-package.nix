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
      ../config/pi-config/src/json.c
      ../config/pi-config/include/json.h
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
  # The fixed capture workload still asks the nested model to write add.py.
  mesonFlags = [ "-Dprobe_python=${pkgs.python3}/bin/python3" ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
