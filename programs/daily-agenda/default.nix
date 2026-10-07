{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "daily-agenda";
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
    curl
    libical
  ];
  mesonFlags = [ "-Dzoneinfo=${pkgs.tzdata}/share/zoneinfo" ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
