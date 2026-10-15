{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  enabled = pkgs.stdenv.isDarwin && hostName == "relay";
  source = "${config.home.homeDirectory}/.local/opt/Strata";
  revision = "296b59d2f73c8f04a805df29c2fa97655adace78";
  data = "${config.xdg.dataHome}/strata";
  keyFile = "${config.xdg.configHome}/local-llm/api-key";
  # Build against Apple's current SDK, not the Nix development shell's older SDK.
  cleanEnvironment = ''
    /usr/bin/env -i HOME="$HOME" USER="$USER" LOGNAME="$USER" \
      PATH=/Library/Developer/CommandLineTools/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin \
      DEVELOPER_DIR=/Library/Developer/CommandLineTools \
      SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
      CC=/Library/Developer/CommandLineTools/usr/bin/clang \
      CXX=/Library/Developer/CommandLineTools/usr/bin/clang++ \
      LANG=en_GB.UTF-8 PYTHONUNBUFFERED=1 \
  '';
  setup = pkgs.writeShellApplication {
    name = "strata-relay-setup";
    runtimeInputs = [ pkgs.git ];
    text = ''
      umask 077
      source=${lib.escapeShellArg source}
      if [ ! -d "$source/.git" ]; then
        mkdir -p "$(dirname "$source")"
        git clone git@github.com:Niko1221/Strata.git "$source"
        git -C "$source" checkout --detach ${revision}
      fi
      if [ "$(git -C "$source" rev-parse HEAD)" != ${revision} ]; then
        echo 'Strata checkout differs from the reviewed revision; refusing to update it implicitly.' >&2
        exit 1
      fi
      cd "$source"
      exec ${cleanEnvironment} STRATA_SOURCE=huggingface PIP_ONLY_BINARY=:all: \
        /bin/sh ./setup.sh --yes --model IQ3_S --context 32768 --vision no \
        --port 8081 --host 127.0.0.1 --no-browser --no-start \
        --data-dir ${lib.escapeShellArg data}
    '';
  };
  run = pkgs.writeShellScript "strata-relay-run" ''
    set -euo pipefail
    umask 077
    source=${lib.escapeShellArg source}
    if [ ! -f "$source/strata-iq3_s.json" ]; then
      echo 'Run strata-relay-setup before starting Strata.' >&2
      exit 0
    fi
    if [ -e ${lib.escapeShellArg "${data}/models/IQ3_S/.preparing.lock"} ]; then
      echo 'Model preparation must finish before Strata starts.' >&2
      exit 1
    fi
    if [ ! -s ${lib.escapeShellArg keyFile} ]; then
      echo 'The Hermes local-model API key is missing.' >&2
      exit 1
    fi
    # Read after the environment reset: the key never appears in process arguments.
    export STRATA_API_KEY
    STRATA_API_KEY="$(< ${lib.escapeShellArg keyFile})"
    # Favor intelligence: do not skip low-ranked routed experts for speed.
    export STRATA_ROUTE_TAIL_SKIP=0
    cd "$source"
    exec "$source/.venv/bin/python" serve/server.py --engine strata \
      --config "$source/strata-iq3_s.json" --host 127.0.0.1 --port 8081 --fit-max-tokens
  '';
  server = pkgs.writeShellApplication {
    name = "strata-relay-server";
    text = ''
      exec ${cleanEnvironment} ${run}
    '';
  };
  job = {
    ProgramArguments = [ "${server}/bin/strata-relay-server" ];
    RunAtLoad = true;
    KeepAlive.SuccessfulExit = false;
    ThrottleInterval = 30;
    ProcessType = "Background";
    StandardOutPath = "${config.home.homeDirectory}/Library/Logs/strata.log";
    StandardErrorPath = "${config.home.homeDirectory}/Library/Logs/strata.log";
  };
in
{
  config = lib.mkIf enabled {
    home.packages = [
      setup
      server
    ];
    dotfiles.darwinDaemons.strata = lib.mkIf config.dotfiles.darwinSystemServices job;
    launchd.agents.strata = lib.mkIf (!config.dotfiles.darwinSystemServices) {
      enable = true;
      config = job;
    };
  };
}
