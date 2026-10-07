{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "opencode-adapt";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [ pkgs.glib ];
  doCheck = false;
  meta = {
    mainProgram = "opencode-adapt-markdown";
    platforms = pkgs.lib.platforms.unix;
  };
}
