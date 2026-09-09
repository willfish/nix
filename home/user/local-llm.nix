{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  isRelay = pkgs.stdenv.isDarwin && hostName == "relay";
  isAndromeda = pkgs.stdenv.isLinux && hostName == "andromeda";
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
  modelAlias = "qwen3.8-27b";
  modelQuant = if isAndromeda then "UD-Q5_K_M" else "UD-Q6_K";
  modelName = "Qwen3.8-27B-${modelQuant}.gguf";
  modelPath = "${modelDir}/${modelName}";
  modelHash =
    if isAndromeda then
      "2de73110cb254cbf09b54b717578dadff12ef1194e7271527e68202f39ba4bfd"
    else
      "c9c206812fbe4ac7b76a729e25928b63f2ae89d37f69da7a71c20aec763cd436";
  modelRevision = "4ca720788d1e01f1bff70c033e0d0028fd02e502";
  modelUrl = "https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/resolve/${modelRevision}/${modelName}";
  ollamaBlob = "${config.home.homeDirectory}/.ollama/models/blobs/sha256-${modelHash}";
  logPath = "${config.home.homeDirectory}/Library/Logs/local-llm.log";
  apiKeyPath = "${config.xdg.configHome}/local-llm/api-key";
  workspace = "${config.home.homeDirectory}/LocalAssistant";
  chatUi = import ./local-llm-ui.nix { inherit pkgs; };
  profilePython = pkgs.python3.withPackages (ps: [ ps.pyyaml ]);
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
      exec ${config.home.homeDirectory}/.local/bin/hermes -p qwen --yolo --in "$PWD" "$@"
    '';
  };
  piAgentDir = "${config.xdg.configHome}/local-llm/pi";
  piSettings = pkgs.writeText "local-qwen-pi-settings.json" (
    builtins.toJSON {
      defaultProvider = hostName;
      defaultModel = modelAlias;
      # Pi's highest level; pi-qwen.js maps it to Qwen's top "xhigh" reasoning effort.
      defaultThinkingLevel = "max";
      enableInstallTelemetry = false;
      compaction = {
        enabled = true;
        reserveTokens = 16384;
        keepRecentTokens = 8192;
      };
      retry.provider = {
        timeoutMs = 1800000;
        maxRetries = 0;
      };
    }
  );
  piSystemPrompt = pkgs.writeText "local-qwen-pi-system.md" ''
    You are a local system administration and coding assistant running on William's ${hostName} computer.
    Use the available tools to inspect the actual machine, run commands and edit files.
    Work in the current directory. Diagnose before changing things, preserve unrelated work,
    and verify your changes. Never claim to have run a tool unless you did.
    Keep replies concise. Never print secrets. Ask before destructive or out-of-scope actions.
    Use existing tools or ephemeral Nix tooling; do not install global dependencies unasked.
    Use the mcp tool for configured external services. Connect to a server before searching
    its tools if the metadata cache is empty. Never send messages or publish changes unasked.
    Do not use em dashes. Internet access may be unavailable; use local evidence when offline.
  '';
  qwenPi = pkgs.writeShellApplication {
    name = "qwen-pi";
    text = ''
      umask 077
      export PI_CODING_AGENT_DIR=${lib.escapeShellArg piAgentDir}
      export PI_TELEMETRY=0
      exec ${pkgs.pi-coding-agent}/bin/pi \
        --offline --provider ${hostName} --model ${modelAlias} \
        --no-context-files --no-skills --no-extensions --no-prompt-templates --no-themes \
        --extension ${../config/local-llm/pi-qwen.js} \
        --extension ${config.home.homeDirectory}/.pi/agent/extensions/mcp/index.ts \
        --extension ${config.home.homeDirectory}/.pi/agent/extensions/todo.ts \
        --prompt-template ${config.home.homeDirectory}/.pi/agent/prompts/plan-work.md \
        --prompt-template ${config.home.homeDirectory}/.pi/agent/prompts/review.md \
        --system-prompt "$(< ${piSystemPrompt})" \
        --tools read,bash,edit,write,mcp,todo "$@"
    '';
  };
  promptCapture = import ./prompt-capture.nix { inherit pkgs; };
  qwenClaude = pkgs.writeShellApplication {
    name = "qwen-claude";
    text = ''
      umask 077
      export CLAUDE_CONFIG_DIR=${lib.escapeShellArg "${config.xdg.configHome}/local-llm/claude"}
      mkdir -p "$CLAUDE_CONFIG_DIR"
      unset CLAUDE_CODE_OAUTH_TOKEN ANTHROPIC_API_KEY
      unset CLAUDE_CODE_USE_BEDROCK CLAUDE_CODE_USE_VERTEX CLAUDE_CODE_USE_FOUNDRY
      export ANTHROPIC_AUTH_TOKEN
      ANTHROPIC_AUTH_TOKEN="$(< ${lib.escapeShellArg apiKeyPath})"
      export ANTHROPIC_BASE_URL=http://127.0.0.1:8081
      export ANTHROPIC_MODEL=${modelAlias}
      export ANTHROPIC_CUSTOM_MODEL_OPTION=${modelAlias}
      export ANTHROPIC_DEFAULT_OPUS_MODEL=${modelAlias}
      export ANTHROPIC_DEFAULT_SONNET_MODEL=${modelAlias}
      export ANTHROPIC_DEFAULT_HAIKU_MODEL=${modelAlias}
      export CLAUDE_CODE_SUBAGENT_MODEL=${modelAlias}
      export CLAUDE_CODE_MAX_CONTEXT_TOKENS=${toString contextSize}
      export CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1
      export DISABLE_AUTOUPDATER=1
      export NO_PROXY="localhost,127.0.0.1''${NO_PROXY:+,$NO_PROXY}"
      export no_proxy="$NO_PROXY"

      if [ -n "''${CAPTURE_PROMPTS-1}" ] && [ "''${CAPTURE_PROMPTS-1}" != "0" ]; then
        export PROMPT_CAPTURE_UPSTREAM="$ANTHROPIC_BASE_URL"
        exec ${promptCapture}/bin/prompt-capture qwen-claude -- ${pkgs.claude-code}/bin/claude "$@"
      fi
      exec ${pkgs.claude-code}/bin/claude "$@"
    '';
  };
  toolsPython = pkgs.python3.withPackages (ps: [
    ps.mcp
    ps.ddgs
    ps.beautifulsoup4
  ]);
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
            "http://192.168.178.55:*"
          ]
        )
      }
      export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
      exec ${toolsPython}/bin/python3 ${../config/local-llm/assistant_tools.py}
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
        echo "Downloading Unsloth ${modelName}."
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
      exec ${llamaCpp}/bin/llama-server \
        --model ${lib.escapeShellArg modelPath} \
        --alias ${modelAlias} \
        --host ${if isAndromeda then "127.0.0.1" else "0.0.0.0"} --port 8081 \
        --api-key-file ${lib.escapeShellArg apiKeyPath} \
        ${lib.optionalString isRelay "--ui-mcp-proxy --ui-config-file ${uiConfig} --path ${chatUi}"} \
        --ctx-size ${toString contextSize} --parallel 1 \
        --n-gpu-layers 99 --flash-attn on \
        ${lib.optionalString isAndromeda "--fit off --batch-size 512 --ubatch-size 128"} \
        --cache-type-k q8_0 --cache-type-v q8_0 --cache-ram 1024 \
        --jinja --reasoning off \
        --chat-template-file ${../config/local-llm/qwen3.8-chat-template.jinja} \
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

  chatKey = pkgs.writeShellApplication {
    name = "local-chat-key";
    text = ''
      /usr/bin/pbcopy < ${lib.escapeShellArg apiKeyPath}
      echo "Login key copied. Paste it into the chat page's API key field."
    '';
  };
in
lib.mkIf (isRelay || isAndromeda) {
  home.packages = [
    qwenPi
    qwenClaude
    fetchModel
    server
  ]
  ++ lib.optionals isRelay [
    pkgs.llama-cpp
    chat
    chatKey
    assistantTools
  ];

  xdg.configFile."local-llm/pi/models.json".text = builtins.toJSON {
    providers.${hostName} = {
      baseUrl = "http://127.0.0.1:8081/v1";
      api = "openai-completions";
      # Resolve at request time, never embed the secret in the Nix store.
      apiKey = "!${pkgs.coreutils}/bin/cat ${lib.escapeShellArg apiKeyPath}";
      models = [
        {
          id = modelAlias;
          name = "Local Qwen 3.8 27B ${modelQuant}";
          reasoning = true;
          input = [ "text" ];
          contextWindow = contextSize;
          maxTokens = 16384;
          compat = {
            supportsStore = false;
            supportsDeveloperRole = false;
            supportsReasoningEffort = false;
            maxTokensField = "max_tokens";
            thinkingFormat = "qwen-chat-template";
          };
        }
      ];
    };
  };

  # Pi writes settings from its UI, so install a writable copy, keeping a backup on changes.
  home.activation.configureLocalPi = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
    piSettingsPath=${lib.escapeShellArg "${piAgentDir}/settings.json"}
    ${pkgs.coreutils}/bin/mkdir -p ${lib.escapeShellArg piAgentDir}
    if [ -e "$piSettingsPath" ] && ! ${pkgs.coreutils}/bin/cmp -s ${piSettings} "$piSettingsPath"; then
      ${pkgs.coreutils}/bin/cp -p "$piSettingsPath" "$piSettingsPath.before-home-manager-$(${pkgs.coreutils}/bin/date +%Y%m%d%H%M%S)"
    fi
    ${pkgs.coreutils}/bin/install -m 0600 ${piSettings} "$piSettingsPath"
  '';

  home.activation.configureLocalHermes = lib.mkIf isRelay (
    lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      if [ -x ${lib.escapeShellArg "${config.home.homeDirectory}/.local/bin/hermes"} ]; then
        ${profilePython}/bin/python3 ${../config/local-llm/hermes_profile.py} \
          ${lib.escapeShellArg "${config.home.homeDirectory}/.hermes/profiles/qwen/config.yaml"} \
          ${hermesOverlay} --key-file ${lib.escapeShellArg apiKeyPath}
        qwenPath=${lib.escapeShellArg "${config.home.homeDirectory}/.local/bin/qwen"}
        if [ -f "$qwenPath" ] && [ ! -e "$qwenPath.before-local-llm" ]; then
          ${pkgs.coreutils}/bin/cp -p "$qwenPath" "$qwenPath.before-local-llm"
        fi
        ${pkgs.coreutils}/bin/install -m 0755 ${qwen}/bin/qwen "$qwenPath"
      fi
    ''
  );

  launchd.agents.local-llm = lib.mkIf isRelay {
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

  launchd.agents.local-assistant-tools = lib.mkIf isRelay {
    enable = true;
    config = {
      ProgramArguments = [ "${assistantTools}/bin/local-assistant-tools" ];
      RunAtLoad = true;
      KeepAlive = true;
      ThrottleInterval = 30;
      StandardOutPath = "${config.home.homeDirectory}/Library/Logs/local-assistant-tools.log";
      StandardErrorPath = "${config.home.homeDirectory}/Library/Logs/local-assistant-tools.log";
    };
  };

  systemd.user.services.local-llm = lib.mkIf isAndromeda {
    Unit = {
      Description = "Local Qwen3.8 27B ${modelQuant} on the NVIDIA GPU";
      After = [ "graphical-session.target" ];
      ConditionPathExists = modelPath;
    };
    Service = {
      ExecStart = "${server}/bin/local-llm-server";
      Restart = "on-failure";
      RestartSec = 5;
      TimeoutStopSec = 30;
      UMask = "0077";
      NoNewPrivileges = true;
    };
    Install.WantedBy = [ "default.target" ];
  };
}
