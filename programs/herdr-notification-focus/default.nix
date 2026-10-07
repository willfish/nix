{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "herdr-notification-focus";
  version = "0.1.0";
  src = ./.;
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
  meta.platforms = pkgs.lib.platforms.linux;
}
