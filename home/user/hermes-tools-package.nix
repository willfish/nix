{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "hermes-tools";
  version = "0.1.0";
  src = ../config/hermes-tools;
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    makeWrapper
  ];
  buildInputs = with pkgs; [
    glib
    yyjson
  ];
  postInstall = ''
    wrapProgram "$out/bin/hermes-export" \
      --suffix PATH : ${pkgs.lib.makeBinPath [ pkgs.sops ]}
  '';
  doCheck = false;
  meta = {
    mainProgram = "hermes-export";
    platforms = pkgs.lib.platforms.unix;
  };
}
