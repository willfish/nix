{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "voice-models";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = with pkgs; [
    glib
    curl
  ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
