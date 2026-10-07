{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "pi-voice-client";
  version = "0.2.0";
  src = ./.;

  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    wrapGAppsHook4
  ];
  buildInputs = with pkgs; [
    yyjson
    curl
    pcre2
    cairo
    pango
    glib
    gtk4
    gtk4-layer-shell
  ];

  mesonBuildType = "debugoptimized";
  doCheck = false;
  meta = {
    description = "Pi voice UI, session integration and speech-service client";
    mainProgram = "pi-voice-c";
    platforms = pkgs.lib.platforms.linux;
  };
}
