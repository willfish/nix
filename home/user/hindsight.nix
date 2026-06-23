{
  config,
  lib,
  pkgs,
  ...
}:
let
  image = "ghcr.io/vectorize-io/hindsight:0.8.3-slim";
  containerName = "hindsight-mcp";
  hindsightSource = pkgs.fetchFromGitHub {
    owner = "vectorize-io";
    repo = "hindsight";
    rev = "e1014cc";
    sha256 = "1lfmbzy8jgyys5n7g3s70zygybh9rkpzsdljgf9szqm42ydg690g";
  };
  codexScriptsDir = "${config.home.homeDirectory}/.hindsight/codex/scripts";
  secretDir = "${config.home.homeDirectory}/.config/sops-nix/secrets";
  sopsSecretHelpers = ''
    secret_dir="${secretDir}"

    read_secret() {
      value="$(<"$1")"
      value="''${value%\"}"
      value="''${value#\"}"
      printf '%s' "$value"
    }
  '';
in
{
  home.file.".local/bin/hindsight-mcp-start" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      ${sopsSecretHelpers}

      HINDSIGHT_API_DATABASE_URL="$(read_secret "$secret_dir/HINDSIGHT_API_DATABASE_URL")"
      HINDSIGHT_API_LLM_API_KEY="$(read_secret "$secret_dir/OPENROUTER_API_KEY")"

      : "''${HINDSIGHT_API_DATABASE_URL:?HINDSIGHT_API_DATABASE_URL must be set in sops-nix secrets}"
      : "''${HINDSIGHT_API_LLM_API_KEY:?OPENROUTER_API_KEY must be set in sops-nix secrets}"

      export HINDSIGHT_API_DATABASE_URL
      export HINDSIGHT_API_LLM_API_KEY
      export HINDSIGHT_API_OPENROUTER_API_KEY="$HINDSIGHT_API_LLM_API_KEY"
      export HINDSIGHT_API_EMBEDDINGS_OPENROUTER_API_KEY="$HINDSIGHT_API_LLM_API_KEY"

      exec ${pkgs.docker_29}/bin/docker run --rm \
        --name ${containerName} \
        --pull missing \
        --publish 127.0.0.1:8888:8888 \
        --publish 127.0.0.1:9999:9999 \
        --env HINDSIGHT_API_DATABASE_URL \
        --env HINDSIGHT_API_LLM_PROVIDER=openrouter \
        --env HINDSIGHT_API_LLM_MODEL=qwen/qwen3.5-9b \
        --env HINDSIGHT_API_LLM_API_KEY \
        --env HINDSIGHT_API_OPENROUTER_API_KEY \
        --env HINDSIGHT_API_EMBEDDINGS_PROVIDER=openrouter \
        --env HINDSIGHT_API_EMBEDDINGS_OPENROUTER_API_KEY \
        --env HINDSIGHT_API_EMBEDDINGS_OPENROUTER_MODEL=perplexity/pplx-embed-v1-0.6b \
        --env HINDSIGHT_API_RERANKER_PROVIDER=rrf \
        --env HINDSIGHT_API_HOST=0.0.0.0 \
        --env HINDSIGHT_API_PORT=8888 \
        --env HINDSIGHT_ENABLE_API=true \
        --env HINDSIGHT_ENABLE_CP=true \
        ${image}
    '';
  };

  home.file.".local/bin/hindsight-mcp-stop" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      ${pkgs.docker_29}/bin/docker stop ${containerName} >/dev/null 2>&1 || true
    '';
  };

  home.file.".hindsight/codex/scripts" = {
    source = "${hindsightSource}/hindsight-integrations/codex/scripts";
    recursive = true;
  };

  home.file.".hindsight/codex/settings.json" = {
    source = "${hindsightSource}/hindsight-integrations/codex/settings.json";
  };

  home.file.".hindsight/codex.json" = {
    force = true;
    text = builtins.toJSON {
      hindsightApiUrl = "http://127.0.0.1:8888";
      bankId = "william-codex";
      bankMission = "You are a coding assistant for William. Retain durable technical decisions, project context, debugging outcomes, repository conventions, and user preferences that help future sessions continue without re-explanation.";
      retainMission = "Extract durable technical decisions, code patterns, debugging solutions, repository context, architecture choices, and stable user preferences. Ignore transient shell output, raw tool traces, routine status chatter, file search/list/read activity, facts that only say the user is working in a repository, secrets, tokens, credentials, and sensitive personal data. Do not retain commodity rates, duty rates, legal tariff values, or other time-sensitive public data unless the user explicitly asks to remember a classification outcome.";
      autoRecall = true;
      autoRetain = true;
      retainMode = "chunked";
      retainEveryNTurns = 10;
      retainOverlapTurns = 1;
      retainToolCalls = false;
      recallBudget = "mid";
      recallMaxTokens = 1200;
      recallTimeout = 10;
      dynamicBankId = true;
      dynamicBankGranularity = [
        "agent"
        "project"
      ];
      bankIdPrefix = "william-";
      agentName = "codex";
      debug = false;
    };
  };

  home.file.".codex/hooks.json" = {
    force = true;
    text = builtins.toJSON {
      hooks = {
        SessionStart = [
          {
            hooks = [
              {
                type = "command";
                command = "${pkgs.python3}/bin/python3 \"${codexScriptsDir}/session_start.py\"";
                timeout = 5;
              }
            ];
          }
        ];
        UserPromptSubmit = [
          {
            hooks = [
              {
                type = "command";
                command = "${pkgs.python3}/bin/python3 \"${codexScriptsDir}/recall.py\"";
                timeout = 12;
              }
            ];
          }
        ];
        Stop = [
          {
            hooks = [
              {
                type = "command";
                command = "${pkgs.python3}/bin/python3 \"${codexScriptsDir}/retain.py\"";
                timeout = 30;
              }
            ];
          }
        ];
      };
    };
  };

  systemd.user.services.hindsight-mcp = lib.mkIf pkgs.stdenv.isLinux {
    Unit = {
      Description = "Hindsight local MCP/API service";
      After = [ "network-online.target" ];
    };

    Service = {
      ExecStart = "${config.home.homeDirectory}/.local/bin/hindsight-mcp-start";
      ExecStop = "${config.home.homeDirectory}/.local/bin/hindsight-mcp-stop";
      Restart = "on-failure";
      RestartSec = 10;
      TimeoutStopSec = 30;
    };

    Install = {
      WantedBy = [ "default.target" ];
    };
  };

  launchd.agents.hindsight-mcp = lib.mkIf pkgs.stdenv.isDarwin {
    enable = true;
    config = {
      ProgramArguments = [ "${config.home.homeDirectory}/.local/bin/hindsight-mcp-start" ];
      RunAtLoad = true;
      KeepAlive = {
        SuccessfulExit = false;
      };
      StandardOutPath = "/tmp/hindsight-mcp.out.log";
      StandardErrorPath = "/tmp/hindsight-mcp.err.log";
    };
  };
}
