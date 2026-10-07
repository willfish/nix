{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "arxiv-status";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [ pkgs.yyjson ];
  doCheck = false;
  meta.platforms = pkgs.lib.platforms.unix;
}
