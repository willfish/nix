{ pkgs }:
pkgs.rustPlatform.buildRustPackage {
  pname = "nm-auto-secret-agent";
  version = "0.1.0";
  src = ../config/nm-auto-secret-agent;
  cargoLock.lockFile = ../config/nm-auto-secret-agent/Cargo.lock;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [
    pkgs.networkmanager
    pkgs.glib
  ];
  NM_AGENT_NMCLI = pkgs.lib.getExe' pkgs.networkmanager "nmcli";
  doCheck = false;
  meta = {
    mainProgram = "nm-auto-secret-agent";
    platforms = pkgs.lib.platforms.linux;
  };
}
