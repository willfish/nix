{ pkgs }:
pkgs.rustPlatform.buildRustPackage {
  pname = "tailscale-open-proxy";
  version = "0.1.0";
  src = ../config/tailscale-proxy;
  cargoLock.lockFile = ../config/tailscale-proxy/Cargo.lock;
  doCheck = false;

  meta = {
    description = "Streaming relay proxy with Tailscale-only credential injection";
    mainProgram = "tailscale-open-proxy";
    platforms = pkgs.lib.platforms.unix;
  };
}
