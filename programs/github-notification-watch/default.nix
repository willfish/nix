{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "github-notification-watch";
  version = "0.1.0";
  src = ./.;
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
  meta = {
    description = "Watch GitHub notifications and deliver desktop alerts";
    mainProgram = "github-notification-watch";
    platforms = pkgs.lib.platforms.unix;
  };
}
