{ pkgs, interpreter }:
pkgs.stdenv.mkDerivation {
  pname = "telegram-login";
  version = "0.1.0";
  src = ./.;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
  ];
  buildInputs = [ pkgs.python3 ];
  mesonFlags = [
    "-Dpython_executable=${pkgs.python3}/bin/python3"
    "-Dsite_packages=${interpreter}/${pkgs.python3.sitePackages}"
  ];
  doCheck = false;
  meta = {
    mainProgram = "telegram-mcp-login";
    platforms = pkgs.lib.platforms.unix;
  };
}
