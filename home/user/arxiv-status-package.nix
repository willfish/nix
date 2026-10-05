{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "arxiv-status";
  version = "0.1.0";
  src = ../config/arxiv-status;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  nativeCheckInputs = [ pkgs.nodejs ];
  buildInputs = [ pkgs.yyjson ];
  doCheck = true;
  meta.platforms = pkgs.lib.platforms.unix;
}
