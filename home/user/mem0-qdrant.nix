{
  config,
  lib,
  pkgs,
  ...
}:
let
  composeSource = ../config/docker-compose/mem0-qdrant;
  composeDir = "${config.xdg.configHome}/mem0-qdrant";
  docker = "${pkgs.docker_29}/bin/docker";
  servicePath = lib.makeBinPath [
    pkgs.bash
    pkgs.coreutils
    pkgs.curl
    pkgs.docker-compose
    pkgs.docker_29
    pkgs.gnused
  ];
  waitForDocker = pkgs.writeShellScript "mem0-qdrant-wait-for-docker" ''
    set -euo pipefail

    for attempt in {1..30}; do
      if ${docker} info >/dev/null 2>&1; then
        exit 0
      fi

      echo "Docker daemon is not ready for mem0-qdrant; retrying ($attempt/30)..." >&2
      ${pkgs.coreutils}/bin/sleep 1
    done

    echo "Docker daemon did not become ready for mem0-qdrant" >&2
    exit 1
  '';
in
{
  xdg.configFile."mem0-qdrant" = {
    source = composeSource;
    recursive = true;
  };

  systemd.user.services.mem0-qdrant = lib.mkIf pkgs.stdenv.isLinux {
    Unit = {
      Description = "Qdrant vector database for Mem0 semantic memory";
      After = [ "default.target" ];
    };

    Service = {
      Type = "oneshot";
      RemainAfterExit = true;
      WorkingDirectory = composeDir;
      Environment = [ "PATH=${servicePath}" ];
      ExecStartPre = waitForDocker;
      ExecStart = "${composeDir}/start.sh";
      ExecStop = "${docker} compose -f ${composeDir}/docker-compose.yaml down";
      TimeoutStartSec = "90s";
      TimeoutStopSec = "30s";
    };

    Install = {
      WantedBy = [ "default.target" ];
    };
  };
}
