{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "daily-workflow";
  version = "0.1.0";
  src = ../config/daily-workflow;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [ pkgs.glib ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
