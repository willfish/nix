{ pkgs }:
pkgs.rustPlatform.buildRustPackage {
  pname = "aws-access-portal";
  version = "0.1.0";
  src = ../config/aws-access-portal;
  cargoLock.lockFile = ../config/aws-access-portal/Cargo.lock;
  doCheck = false;
  meta = {
    mainProgram = "aws-access-portal";
    platforms = pkgs.lib.platforms.unix;
  };
}
