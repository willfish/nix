{ pkgs }:
pkgs.rustPlatform.buildRustPackage {
  pname = "pi-voice-api";
  version = "0.1.0";
  src = ./.;
  cargoLock.lockFile = ./Cargo.lock;

  doCheck = false;

  meta = {
    description = "Loopback voice REST adapter for Whisper and audio.cpp";
    mainProgram = "pi-voice-api";
    platforms = pkgs.lib.platforms.unix;
  };
}
