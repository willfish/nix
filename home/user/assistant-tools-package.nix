{ pkgs }:
let
  searchProvider = import ./assistant-search-provider.nix { inherit pkgs; };
in
pkgs.rustPlatform.buildRustPackage {
  pname = "local-assistant-tools";
  version = "0.1.0";
  src = ../config/assistant-tools;
  cargoLock.lockFile = ../config/assistant-tools/Cargo.lock;
  doCheck = false;
  passthru = { inherit searchProvider; };
  SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";

  meta = {
    description = "Scoped filesystem and public-web MCP tools for local chat";
    mainProgram = "local-assistant-tools";
    platforms = pkgs.lib.platforms.unix;
  };
}
