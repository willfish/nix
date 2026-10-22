{ pkgs }:
pkgs.writeShellApplication {
  name = "upstream-status";
  runtimeInputs = [
    pkgs.curl
    pkgs.jq
    pkgs.coreutils
    pkgs.gnused
    pkgs.gnugrep
  ];
  text = builtins.readFile ./upstream-status.sh;
  meta = {
    description = "Report pinned upstreams that trail their repositories";
    mainProgram = "upstream-status";
    platforms = pkgs.lib.platforms.unix;
  };
}
