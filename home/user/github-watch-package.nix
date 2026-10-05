{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "github-watch";
  version = "0.1.0";
  src = ../config/github-watch;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    makeWrapper
  ];
  buildInputs = with pkgs; [
    yyjson
    glib
  ];
  doCheck = false;
  postInstall = ''
    wrapProgram "$out/bin/github-notification-watch" --prefix PATH : "${
      pkgs.lib.makeBinPath [
        pkgs.gh
        pkgs.libnotify
        pkgs.xdg-utils
      ]
    }"
  '';
  meta.platforms = pkgs.lib.platforms.unix;
}
