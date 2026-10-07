{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "login-theme-apply";
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
  meta = {
    mainProgram = "greeter-select";
    platforms = pkgs.lib.platforms.linux;
  };
}
