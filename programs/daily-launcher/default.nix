{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "daily-launcher";
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
    description = "Launch workspace, notes, agenda and confirmed cleanup actions";
    mainProgram = "daily-workflow";
    platforms = pkgs.lib.platforms.unix;
  };
}
