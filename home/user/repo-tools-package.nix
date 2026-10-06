{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "repo-tools";
  version = "0.1.0";
  src = ../config/repo-tools;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = with pkgs; [
    glib
    yyjson
    libxml2
  ];
  doCheck = false;
  meta = {
    mainProgram = "check-flake-lock-update";
    platforms = pkgs.lib.platforms.unix;
  };
}
