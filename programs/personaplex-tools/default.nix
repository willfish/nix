{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "personaplex-tools";
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
    libarchive
    yyjson
  ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
