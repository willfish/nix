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
in
lib.mkIf config.dotfiles.capabilities.memory {
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
}
