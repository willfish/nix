{
  config,
  lib,
  pkgs,
  readSopsSecret,
  ...
}:
let
  imageRepo = "ghcr.io/vectorize-io/hindsight";
  imageTag = "0.8.3-slim";
  imageDigest = "sha256:21532405da3e974a878335bd5734008f93c6066ac99dddda47e474cdc67a6351";
  image = "${imageRepo}:${imageTag}@${imageDigest}";
  containerName = "hindsight-mcp";
  hindsightSource = pkgs.fetchFromGitHub {
    owner = "vectorize-io";
    repo = "hindsight";
    rev = "e1014cc";
    sha256 = "1lfmbzy8jgyys5n7g3s70zygybh9rkpzsdljgf9szqm42ydg690g";
  };
in
{
  home.file.".local/bin/hindsight-mcp-start" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      HINDSIGHT_API_DATABASE_URL="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.HINDSIGHT_API_DATABASE_URL.path})"
      HINDSIGHT_API_LLM_API_KEY="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.OPENROUTER_API_KEY.path})"

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
      autoRecall = false;
      autoRetain = false;
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
      hooks = { };
    };
  };
}
