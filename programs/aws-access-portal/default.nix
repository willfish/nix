{ pkgs }:
pkgs.rustPlatform.buildRustPackage {
  pname = "aws-access-portal";
  version = "0.1.0";
  src = ./.;
  cargoLock.lockFile = ./Cargo.lock;
  doCheck = false;
  meta = {
    mainProgram = "aws-access-portal";
    platforms = pkgs.lib.platforms.unix;
  };
}
