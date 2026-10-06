{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  isAutomationDarwin = pkgs.stdenv.isDarwin && config.dotfiles.role == "automation";
  isAndromeda = pkgs.stdenv.isLinux && hostName == "andromeda";
  isRelay = isAutomationDarwin && hostName == "relay";
  llamaCpp =
    if isAndromeda then
      (pkgs.llama-cpp.override {
        cudaSupport = true;
        inherit (pkgs) cudaPackages;
      }).overrideAttrs
        (previous: {
          # Andromeda's Blackwell GPU only; avoid compiling unused architectures.
          cmakeFlags =
            builtins.filter (flag: !(lib.hasPrefix "-DCMAKE_CUDA_ARCHITECTURES" flag)) previous.cmakeFlags
            ++ [ "-DCMAKE_CUDA_ARCHITECTURES=120" ];
        })
    else
      pkgs.llama-cpp;
  contextSize = if isAndromeda then 131072 else 65536;
  modelDir = "${config.xdg.dataHome}/local-llm";
  modelAlias = if isRelay then "huihui-qwen3.6-35b-a3b" else "qwen3.8-27b";
  modelQuant =
    if isRelay then
      "Q5_K_M"
    else if isAndromeda then
      "UD-Q5_K_XL"
    else
      "UD-Q6_K";
  modelName =
    if isRelay then
      "Huihui-Qwen3.6-35B-A3B-abliterated.${modelQuant}.gguf"
    else
      "Huihui-Qwen3.8-27B-abliterated-${modelQuant}.gguf";
  modelPath = "${modelDir}/${modelName}";
  modelHash =
    if isRelay then
      "0b9660729ffe997d3ac5510689f27c724008c6f3acbb9d1f05dfebde7096f807"
    else if isAndromeda then
      "a6ff520853eba5cad302a2a16144b7fe683792cca7fb2830a08479e78ebe12b6"
    else
      "c9c206812fbe4ac7b76a729e25928b63f2ae89d37f69da7a71c20aec763cd436";
  modelRevision =
    if isRelay then
      "7feb7bae6beaaf314ef087d552a09d8da05e0980"
    else
      "8f1b52408a2f6e317535190c9386f776cacf0079";
  modelRepository =
    if isRelay then
      "mradermacher/Huihui-Qwen3.6-35B-A3B-abliterated-GGUF"
    else
      "huihui-ai/Huihui-Qwen3.8-27B-abliterated-GGUF";
  modelUrl = "https://huggingface.co/${modelRepository}/resolve/${modelRevision}/${modelName}";
  ollamaBlob = "${config.home.homeDirectory}/.ollama/models/blobs/sha256-${modelHash}";
  logPath = "${config.home.homeDirectory}/Library/Logs/local-llm.log";
  apiKeyPath = "${config.xdg.configHome}/local-llm/api-key";
  strata = import ./strata.nix {
    inherit
      config
      lib
      pkgs
      contextSize
      apiKeyPath
      ;
  };
  workspace = "${config.home.homeDirectory}/LocalAssistant";
  chatUi = import ./local-llm-ui.nix { inherit pkgs; };
  profilePython = pkgs.python3.withPackages (ps: [ ps.pyyaml ]);
  tailscaleProxy = import ./tailscale-proxy-package.nix { inherit pkgs; };
  hermesOverlay = pkgs.writeText "local-qwen-hermes.json" (
    builtins.toJSON {
      model = {
        default = modelAlias;
        provider = "qwen-local";
        base_url = "http://127.0.0.1:8081/v1";
        api_key = "";
        context_length = 65536;
      };
      custom_providers = [
        {
          name = "qwen-local";
          base_url = "http://127.0.0.1:8081/v1";
          api_key = "";
          key_env = "LOCAL_QWEN_API_KEY";
          api_mode = "chat_completions";
          extra_body = {
            chat_template_kwargs = {
              enable_thinking = true;
              preserve_thinking = true;
              reasoning_effort = "medium";
            };
            temperature = 1.0;
            top_p = 0.95;
            top_k = 20;
            min_p = 0.0;
            presence_penalty = 0.0;
            repetition_penalty = 1.0;
          };
        }
      ];
      agent = {
        max_turns = "unlimited";
        reasoning_effort = "medium";
      };
      terminal = {
        backend = "local";
        cwd = ".";
      };
      platform_toolsets.cli = [
        "terminal"
        "file"
      ];
      compression = {
        enabled = true;
        threshold = 0.75;
        # Hermes floors small-window percentage triggers; enforce 75% of 64K.
        threshold_tokens = 49152;
        progress_notices = true;
        abort_on_summary_failure = true;
      };
      auxiliary = {
        compression = {
          provider = "main";
          model = "";
          base_url = "";
          api_key = "";
          timeout = 1800;
          reasoning_effort = "none";
          fallback_chain = [ ];
          extra_body = {
            chat_template_kwargs = {
              enable_thinking = false;
              preserve_thinking = true;
            };
            temperature = 0.7;
            top_p = 0.8;
            presence_penalty = 1.5;
          };
        };
        title_generation = {
          enabled = false;
          provider = "none";
        };
        background_review = {
          enabled = false;
          provider = "none";
        };
        triage_specifier.provider = "none";
        curator.provider = "none";
      };
      fallback_providers = [ ];
      fallback_model = null;
      display = {
        streaming = true;
        show_reasoning = true;
      };
    }
  );
  qwen = pkgs.writeShellApplication {
    name = "qwen";
    text = ''
      export LOCAL_QWEN_API_KEY
      LOCAL_QWEN_API_KEY="$(< ${lib.escapeShellArg apiKeyPath})"
      export TERMINAL_CWD="$PWD"
      exec ${
        if isRelay then
          "${pkgs.hermes-agent}/bin/hermes"
        else
          "${config.home.homeDirectory}/.local/bin/hermes"
      } -p qwen --yolo --in "$PWD" "$@"
    '';
  };
  # DDGS remains the third-party search provider, invoked on demand.
  toolsPython = assistantPackage.searchProvider;
  assistantPackage = import ./assistant-tools-package.nix { inherit pkgs; };
  uiConfig = pkgs.writeText "local-llm-ui.json" (
    builtins.toJSON {
      mcpServers = builtins.toJSON [
        {
          id = "relay-assistant";
          name = "Web and files";
          enabled = true;
          url = "http://127.0.0.1:8082/mcp";
          useProxy = true;
          requestTimeoutSeconds = 60;
        }
      ];
    }
  );

  assistantTools = pkgs.writeShellApplication {
    name = "local-assistant-tools";
    runtimeInputs = [ pkgs.coreutils ];
    text = ''
      umask 077
      mkdir -p ${lib.escapeShellArg workspace}
      export LOCAL_ASSISTANT_READ_ROOTS=${
        lib.escapeShellArg (
          builtins.toJSON [
            "${config.home.homeDirectory}/Repositories"
            "${config.home.homeDirectory}/Notes"
            "${config.home.homeDirectory}/.dotfiles"
          ]
        )
      }
      export LOCAL_ASSISTANT_WRITE_ROOT=${lib.escapeShellArg workspace}
      export LOCAL_ASSISTANT_ALLOWED_ORIGINS=${
        lib.escapeShellArg (
          builtins.toJSON [
            "http://127.0.0.1:*"
            "http://localhost:*"
            "http://relay:*"
            "http://relay.local:*"
            "http://relay.fritz.box:*"
            "http://relay.taile09696.ts.net:*"
            "http://192.168.178.55:*"
          ]
        )
      }
      export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
      export LOCAL_ASSISTANT_SEARCH_BIN=${toolsPython}/bin/ddgs
      exec ${assistantPackage}/bin/local-assistant-tools
    '';
  };

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
        echo "Downloading ${modelName}."
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
    runtimeInputs = [
      pkgs.coreutils
      pkgs.openssl
    ];
    text = ''
      if [ ! -r ${lib.escapeShellArg modelPath} ]; then
        echo "Model missing. Run local-llm-fetch first." >&2
        exit 0
      fi
      umask 077
      mkdir -p ${lib.escapeShellArg "${config.xdg.configHome}/local-llm"}
      if [ ! -s ${lib.escapeShellArg apiKeyPath} ]; then
        openssl rand -hex 32 > ${lib.escapeShellArg apiKeyPath}
      fi
      chmod 600 ${lib.escapeShellArg apiKeyPath}
      # Bind every address. NixOS trusts tailscale0 and does not open 8081 on the
      # LAN, so Andromeda is tailnet-only. Relay already served LAN plus Tailscale.
      # Authenticate with the API key. Do not enable Tailscale Funnel.
      # llama.cpp cannot exempt one interface. Relay's public port is a proxy
      # that injects the key for Tailscale clients only.
      run_llama() {
        exec ${llamaCpp}/bin/llama-server \
          --model ${lib.escapeShellArg modelPath} \
          --alias ${modelAlias} \
          --host "$1" --port "$2" \
        --api-key-file ${lib.escapeShellArg apiKeyPath} \
        ${lib.optionalString isAutomationDarwin "--ui-mcp-proxy --ui-config-file ${uiConfig} --path ${chatUi}"} \
        --ctx-size ${toString contextSize} --parallel 1 \
        --n-gpu-layers 99 --flash-attn on \
        ${lib.optionalString isAndromeda "--fit off --batch-size 512 --ubatch-size 128"} \
        --cache-type-k q8_0 --cache-type-v q8_0 --cache-ram 1024 \
        --jinja --reasoning off \
        ${
          lib.optionalString (
            !isRelay
          ) "--chat-template-file ${../config/local-llm/qwen3.8-chat-template.jinja}"
        } \
        --chat-template-kwargs '${
          builtins.toJSON (
            if isRelay then
              { enable_thinking = false; }
            else
              {
                preserve_thinking = true;
                reasoning_effort = "medium";
              }
          )
        }' \
        --temp 0.7 --top-p 0.8 --top-k 20 --min-p 0 \
        --presence-penalty 1.5 --repeat-penalty 1.0 \
          --sleep-idle-seconds 600 \
          "''${@:3}"
      }
      ${
        if isRelay then
          ''
            run_llama 127.0.0.1 18081 "$@" &
            llama_pid=$!
            ${tailscaleProxy}/bin/tailscale-open-proxy \
              --listen-host 0.0.0.0 --listen-port 8081 \
              --upstream-host 127.0.0.1 --upstream-port 18081 \
              --key-file ${lib.escapeShellArg apiKeyPath} &
            proxy_pid=$!
            # shellcheck disable=SC2329
            cleanup() {
              kill "$llama_pid" "$proxy_pid" 2>/dev/null || true
            }
            trap cleanup EXIT INT TERM
            while kill -0 "$llama_pid" 2>/dev/null && kill -0 "$proxy_pid" 2>/dev/null; do
              sleep 1
            done
            exit 1
          ''
        else
          ''
            run_llama 0.0.0.0 8081 "$@"
          ''
      }
    '';
  };

  chat = pkgs.writeShellApplication {
    name = "local-chat";
    text = ''
      exec /usr/bin/open http://127.0.0.1:8081
    '';
  };

  chatKey = pkgs.writeShellApplication {
    name = "local-chat-key";
    text = ''
      /usr/bin/pbcopy < ${lib.escapeShellArg apiKeyPath}
      echo "Login key copied. Paste it into the chat page's API key field."
    '';
  };
in
lib.mkIf (isAutomationDarwin || isAndromeda) {
  dotfiles.hermes.qwenOverlay = lib.mkIf isRelay "${hermesOverlay}";
  home.packages = [
    fetchModel
    server
  ]
  ++ lib.optionals isAndromeda [
    strata.server
    strata.fetch
  ]
  ++ lib.optionals isAutomationDarwin [
    pkgs.llama-cpp
    chat
    chatKey
    assistantTools
  ];

  home.activation.configureLocalHermes = lib.mkIf isAutomationDarwin (
    lib.hm.dag.entryAfter [ "writeBoundary" "configureHermesDeclaration" ] ''
      if [ -x ${
        lib.escapeShellArg (
          if isRelay then
            "${pkgs.hermes-agent}/bin/hermes"
          else
            "${config.home.homeDirectory}/.local/bin/hermes"
        )
      } ]; then
        ${lib.optionalString (!isRelay) ''
          ${profilePython}/bin/python3 ${../config/local-llm/hermes_profile.py} \
            ${lib.escapeShellArg "${config.home.homeDirectory}/.hermes/profiles/qwen/config.yaml"} \
            ${hermesOverlay} --key-file ${lib.escapeShellArg apiKeyPath}
        ''}
        qwenPath=${lib.escapeShellArg "${config.home.homeDirectory}/.local/bin/qwen"}
        if [ -f "$qwenPath" ] && [ ! -e "$qwenPath.before-local-llm" ]; then
          ${pkgs.coreutils}/bin/cp -p "$qwenPath" "$qwenPath.before-local-llm"
        fi
        ${pkgs.coreutils}/bin/install -m 0755 ${qwen}/bin/qwen "$qwenPath"
      fi
    ''
  );

  dotfiles.darwinDaemons = lib.mkIf (isAutomationDarwin && config.dotfiles.darwinSystemServices) {
    local-llm = config.launchd.agents.local-llm.config;
    local-assistant-tools = config.launchd.agents.local-assistant-tools.config;
  };

  launchd.agents.local-llm = lib.mkIf isAutomationDarwin {
    enable = !config.dotfiles.darwinSystemServices;
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

  launchd.agents.local-assistant-tools = lib.mkIf isAutomationDarwin {
    enable = !config.dotfiles.darwinSystemServices;
    config = {
      ProgramArguments = [ "${assistantTools}/bin/local-assistant-tools" ];
      RunAtLoad = true;
      KeepAlive = true;
      ThrottleInterval = 30;
      StandardOutPath = "${config.home.homeDirectory}/Library/Logs/local-assistant-tools.log";
      StandardErrorPath = "${config.home.homeDirectory}/Library/Logs/local-assistant-tools.log";
    };
  };

  home.activation.removeHuihuiTrialDropin = lib.mkIf isAndromeda (
    lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      dropin="$XDG_RUNTIME_DIR/systemd/user/local-llm.service.d/90-huihui-trial.conf"
      if [ -f "$dropin" ]; then
        rm -f "$dropin"
        rmdir "$XDG_RUNTIME_DIR/systemd/user/local-llm.service.d" 2>/dev/null || true
      fi
    ''
  );

  systemd.user.services.local-llm = lib.mkIf isAndromeda {
    Unit = {
      Description = "Local Qwen3.8 Flash-Next ${strata.quant} with Strata (128K context)";
      After = [ "graphical-session.target" ];
      ConditionPathExists = strata.readyPath;
    };
    Service = {
      ExecStart = "${strata.server}/bin/strata-server";
      Restart = "on-failure";
      RestartSec = 5;
      TimeoutStopSec = 30;
      UMask = "0077";
      NoNewPrivileges = true;
    };
    Install.WantedBy = [ "default.target" ];
  };

}
