{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "panel-settings";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [ pkgs.glib ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.linux;
}
