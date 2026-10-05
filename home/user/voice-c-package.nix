{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "pi-voice-native";
  version = "0.2.0";
  src = ../config/voice-c;

  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    nodejs
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
  doCheck = true;
}
