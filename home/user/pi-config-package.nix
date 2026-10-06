{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "pi-config";
  version = "0.1.0";
  src = ../config/pi-config;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [ pkgs.glib ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
