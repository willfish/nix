{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  modelDir = "${config.xdg.dataHome}/local-llm";
  modelName = "Qwen3.6-35B-A3B.Q4_K_S.gguf";
  modelPath = "${modelDir}/${modelName}";
  modelHash = "97cc5853d3c163c9cb81782ffc8ee18d909c67f8a3c6b4816b98f1103125c6c6";
  modelRevision = "e7ea2ee70544360507f36ff9a8d8a7b48c67b64d";
  modelUrl = "https://huggingface.co/mradermacher/Qwen3.6-35B-A3B-GGUF/resolve/${modelRevision}/${modelName}";
  ollamaBlob = "${config.home.homeDirectory}/.ollama/models/blobs/sha256-${modelHash}";
  logPath = "${config.home.homeDirectory}/Library/Logs/local-llm.log";

  fetchModel = pkgs.writeShellApplication {
    name = "local-llm-fetch";
    runtimeInputs = [
      pkgs.coreutils
      pkgs.curl
    ];
    text = ''
      model=${lib.escapeShellArg modelPath}
      if [ -f "$model" ]; then
        echo "Model already available: $model"
        exit 0
      fi

      mkdir -p ${lib.escapeShellArg modelDir}
      if [ -f ${lib.escapeShellArg ollamaBlob} ]; then
        echo "Verifying and reusing the existing Ollama model (no extra disk space)."
        printf '%s  %s\n' ${lib.escapeShellArg modelHash} ${lib.escapeShellArg ollamaBlob} | sha256sum --check
        # An independent hard link survives deletion from Ollama's model library.
        ln ${lib.escapeShellArg ollamaBlob} "$model"
      else
        echo "Downloading Qwen3.6-35B-A3B Q4_K_S (19.9 GB)."
        curl --fail --location --retry 3 --continue-at - \
          --output "$model.partial" ${lib.escapeShellArg modelUrl}
        printf '%s  %s\n' ${lib.escapeShellArg modelHash} "$model.partial" | sha256sum --check
        mv -n "$model.partial" "$model"
      fi
      echo "Model available: $model"
    '';
  };

  server = pkgs.writeShellApplication {
    name = "local-llm-server";
    text = ''
      if [ ! -r ${lib.escapeShellArg modelPath} ]; then
        echo "Model missing. Run local-llm-fetch first." >&2
        exit 0
      fi
      exec ${pkgs.llama-cpp}/bin/llama-server \
        --model ${lib.escapeShellArg modelPath} \
        --alias qwen3.6-35b-a3b \
        --host 0.0.0.0 --port 8081 \
        --ctx-size 65536 --parallel 1 \
        --n-gpu-layers 99 --flash-attn on \
        --cache-type-k q8_0 --cache-type-v q8_0 --cache-ram 1024 \
        --jinja --reasoning off \
        --chat-template-kwargs '{"preserve_thinking":true}' \
        --temp 0.7 --top-p 0.8 --top-k 20 --min-p 0 \
        --presence-penalty 1.5 --repeat-penalty 1.0 \
        --sleep-idle-seconds 600 \
        "$@"
    '';
  };

  chat = pkgs.writeShellApplication {
    name = "local-chat";
    text = ''
      exec /usr/bin/open http://127.0.0.1:8081
    '';
  };
in
lib.mkIf (pkgs.stdenv.isDarwin && hostName == "relay") {
  home.packages = [
    pkgs.llama-cpp
    fetchModel
    server
    chat
  ];

  launchd.agents.local-llm = {
    enable = true;
    config = {
      ProgramArguments = [ "${server}/bin/local-llm-server" ];
      RunAtLoad = true;
      KeepAlive.PathState.${modelPath} = true;
      ThrottleInterval = 30;
      ProcessType = "Interactive";
      StandardOutPath = logPath;
      StandardErrorPath = logPath;
    };
  };
}
