{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "greeter-select";
  version = "0.1.0";
  src = ../../home/config/greeter-select;
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
  meta = {
    mainProgram = "greeter-select";
    platforms = pkgs.lib.platforms.linux;
  };
}
