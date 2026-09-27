{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  isServer = pkgs.stdenv.isLinux && hostName == "terminus";
  package = pkgs.arxiv-library;
  root = "/srv/media/arxiv";
  environment = [
    "OPENBLAS_NUM_THREADS=2"
    "OMP_NUM_THREADS=2"
    "RAYON_NUM_THREADS=2"
  ];
  worker = command: {
    Unit = {
      Description = "arXiv library ${command}";
      After = [
        "network-online.target"
        "arxiv-prepare.service"
      ];
      Requires = [ "arxiv-prepare.service" ];
      # Preserve checkpointed jobs across Home Manager switches. New binaries
      # take effect on the next start, rather than restarting a multi-day job.
      "X-RestartIfChanged" = false;
    };
    Service = {
      # These jobs may run for days. Their process lifetime must not hold the
      # Home Manager activation transaction open like a oneshot start job.
      Type = "simple";
      # Adopting an already-activating oneshot must not retroactively apply
      # systemd's default startup timeout when its definition is reloaded.
      TimeoutStartSec = "infinity";
      ExecStart = "${package}/bin/arxiv-library-${command} ${root}";
      Environment = environment;
      Nice = 19;
      Restart = "on-failure";
      RestartSec = 300;
      MemoryMax = "2G";
    };
    Install.WantedBy = [ "default.target" ];
  };
in
{
  home.packages = lib.optionals isServer [
    package
    pkgs.arxiv-mcp
  ];

  home.file.".local/bin/mcp-arxiv" = lib.mkIf config.dotfiles.privateEnabled {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail
      ${
        if isServer then
          ''
            export ARXIV_DATABASE=${root}/library.sqlite3
            exec ${pkgs.arxiv-mcp}/bin/arxiv-mcp
          ''
        else
          ''
            exec ${pkgs.openssh}/bin/ssh -T -o BatchMode=yes -o ConnectTimeout=10 \
              -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
              william@terminus.fritz.box /home/william/.local/bin/mcp-arxiv
          ''
      }
    '';
  };

  systemd.user.services = lib.mkIf isServer {
    arxiv-prepare = {
      Unit = {
        Description = "Prepare pinned arXiv corpus manifest and scientific embedding model";
        "X-RestartIfChanged" = false;
      };
      Service = {
        Type = "oneshot";
        ExecStart = "${package}/bin/arxiv-library-prepare ${root}";
        RemainAfterExit = true;
        Environment = environment;
      };
    };
    arxiv-download = lib.recursiveUpdate (worker "download") {
      # Avoid re-reading all 70 GB after every switch once acquisition succeeds.
      Service.RemainAfterExit = true;
    };
    arxiv-ingest = lib.recursiveUpdate (worker "ingest") {
      Service.ExecStart = "${package}/bin/arxiv-library-ingest ${root} --watch";
    };
    arxiv-embeddings = lib.recursiveUpdate (worker "embeddings") {
      # Embeddings are opt-in; routine ingestion only builds the BM25 index.
      Install.WantedBy = [ ];
      Service = {
        ExecStart = "${package}/bin/arxiv-library-embeddings ${root} --watch";
        CPUQuota = "150%";
      };
    };
    arxiv-index = {
      Unit = {
        Description = "Publish compact arXiv passage search index";
        After = [ "arxiv-prepare.service" ];
        Requires = [ "arxiv-prepare.service" ];
        "X-RestartIfChanged" = false;
        ConditionPathExists = "${root}/embeddings/config.json";
      };
      Service = {
        Type = "oneshot";
        ExecStart = "${package}/bin/arxiv-library-index ${root}";
        Environment = environment;
        Nice = 19;
        CPUQuota = "100%";
        MemoryMax = "6G";
      };
    };
  };
  systemd.user.timers.arxiv-index = lib.mkIf isServer {
    Unit.Description = "Refresh arXiv passage search coverage hourly";
    Timer = {
      OnBootSec = "10min";
      OnUnitInactiveSec = "1h";
      Unit = "arxiv-index.service";
    };
    # Retain manual use without scheduling further embedding publication.
    Install.WantedBy = [ ];
  };
}
