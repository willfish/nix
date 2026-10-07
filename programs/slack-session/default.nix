{ pkgs }:
pkgs.rustPlatform.buildRustPackage {
  pname = "slack-session";
  version = "0.1.0";
  src = pkgs.lib.fileset.toSource {
    root = ../.;
    fileset = pkgs.lib.fileset.unions [
      ./.
      ../aws-access-portal/src/cdp.rs
    ];
  };
  sourceRoot = "source/slack-session";
  cargoLock.lockFile = ./Cargo.lock;
  doCheck = false;
  meta = {
    mainProgram = "slack-refresh-session";
    platforms = pkgs.lib.platforms.unix;
  };
}
