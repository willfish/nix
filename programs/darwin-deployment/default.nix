{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "darwin-deployment";
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
    libgit2
  ];
  doCheck = false;
  meta = {
    mainProgram = "darwin-health";
    platforms = pkgs.lib.platforms.unix;
  };
}
