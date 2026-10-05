{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "wallpaper-cycle";
  version = "0.1.0";
  src = ../config/wallpaper-cycle;
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
