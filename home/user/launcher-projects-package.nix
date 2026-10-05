{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "launcher-projects";
  version = "0.1.0";
  src = ../config/launcher-projects;
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
