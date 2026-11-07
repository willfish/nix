{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  enabled = config.dotfiles.privateEnabled;
  isServer = pkgs.stdenv.isLinux && hostName == "terminus";
  isLocalClient = pkgs.stdenv.isLinux && config.dotfiles.capabilities.desktop;
  package = import ../../programs/book-library { inherit pkgs; };
  root = "/srv/media/knowledge";
  essentials = "${config.xdg.dataHome}/book-library/essentials";
  maintain = pkgs.writeShellApplication {
    name = "book-library-maintain";
    runtimeInputs = [
      package
      pkgs.jq
    ];
    text = ''
      export BOOK_LIBRARY_ROOT=${lib.escapeShellArg root}
      test -f "$BOOK_LIBRARY_ROOT/manifest.json"
      while [ -f "$BOOK_LIBRARY_ROOT/acquisition-enabled" ]; do
        download_status=0
        # Publish each small reference before a later full Wikipedia transfer.
        book-library acquire --limit 1 || download_status=$?
        index_status=0
        book-library ingest --limit 1 || index_status=$?
        if [ "$download_status" -ne 0 ] || [ "$index_status" -ne 0 ]; then
          exit 1
        fi
        remaining=$(book-library status | jq '[.states.pending // 0, .states.failed // 0, .states.verified // 0] | add')
        if [ "$remaining" -eq 0 ]; then
          book-library audit --quick
          exit 0
        fi
      done
    '';
  };
in
{
  home.packages = lib.optionals (enabled && pkgs.stdenv.isLinux) [ package ];

  home.file.".local/bin/mcp-books" = lib.mkIf enabled {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail
      ${
        if isServer then
          ''
            exec ${package}/bin/book-library --root ${lib.escapeShellArg root} mcp
          ''
        else
          ''
            exec ${pkgs.openssh}/bin/ssh -T -o BatchMode=yes -o ConnectTimeout=10 \
              -o ServerAliveInterval=30 -o ServerAliveCountMax=3 \
              william@terminus.fritz.box /home/william/.local/bin/mcp-books
          ''
      }
    '';
  };

  home.file.".local/bin/mcp-books-local" = lib.mkIf (enabled && isLocalClient) {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail
      exec ${package}/bin/book-library --root ${lib.escapeShellArg essentials} mcp
    '';
  };

  systemd.user.services.book-library-maintain = lib.mkIf (enabled && isServer) {
    Unit = {
      Description = "Acquire and index the explicitly enabled offline knowledge corpus";
      ConditionPathExists = "${root}/acquisition-enabled";
      "X-RestartIfChanged" = false;
    };
    Service = {
      Type = "simple";
      ExecStart = lib.getExe maintain;
      Environment = [
        "TMPDIR=${root}/scratch"
        "XDG_CACHE_HOME=${root}/cache"
        "OMP_NUM_THREADS=1"
        "RAYON_NUM_THREADS=1"
      ];
      Nice = 19;
      CPUQuota = "100%";
      MemoryMax = "2G";
      IOWeight = 10;
      Restart = "on-failure";
      RestartSec = 300;
      UMask = "0077";
      NoNewPrivileges = true;
    };
    # No WantedBy: activating a home must never start a bulk acquisition.
  };
}
