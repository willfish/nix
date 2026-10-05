{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "voice-c-check";
  version = "0.1.0";
  src = ../home/config/voice-c;

  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    python3
  ];
  buildInputs = with pkgs; [
    yyjson
    curl
    pcre2
    cairo
    pango
    glib
  ];

  doCheck = true;
  mesonBuildType = "debugoptimized";
  installPhase = ''
    runHook preInstall
    touch "$out"
    runHook postInstall
  '';
}
