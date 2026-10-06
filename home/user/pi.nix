{
  config,
  lib,
  pkgs,
  hostName,
  readSopsSecret,
  piThemeArgs,
  ...
}:
let
  piConfig = import ./pi-config-package.nix { inherit pkgs; };
  voiceFeatures = import ./voice-supported.nix { inherit pkgs hostName; };
  llmMcps = import ./llm-mcps.nix { inherit config; };
  mcpAdapter = pkgs.callPackage ./mcp-packages/pi-mcp-adapter.nix { };
  promptHistory = pkgs.callPackage ./pi-packages/prompt-history.nix { };
  # Preserve sibling imports in the store, not just in Home Manager's symlink tree.
  piExtensions = ../config/pi/extensions;
  promptCapture = import ./prompt-capture.nix { inherit pkgs; };
  sopsApiKey =
    name:
    "!${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.${name}.path}";
  qwenBaseUrl =
    host:
    if hostName == host then "http://127.0.0.1:8081/v1" else "http://${host}.taile09696.ts.net:8081/v1";
  # Preserve the source document's layout; builtins.toJSON would minify it.
  piModelsJson = pkgs.runCommand "pi-models.json" { src = ../config/pi/models.json; } ''
    ${pkgs.jq}/bin/jq \
      --arg go ${lib.escapeShellArg (sopsApiKey "OPENCODE_GO_KEY")} \
      --arg relay ${lib.escapeShellArg (sopsApiKey "LOCAL_LLM_RELAY_API_KEY")} \
      --arg andromeda ${lib.escapeShellArg (sopsApiKey "LOCAL_LLM_ANDROMEDA_API_KEY")} \
      --arg relayUrl ${lib.escapeShellArg (qwenBaseUrl "relay")} \
      --arg andromedaUrl ${lib.escapeShellArg (qwenBaseUrl "andromeda")} \
      '.providers["opencode-go"].apiKey = $go
       | .providers.relay.apiKey = $relay
       | .providers.andromeda.apiKey = $andromeda
       | .providers.relay.baseUrl = $relayUrl
       | .providers.andromeda.baseUrl = $andromedaUrl' \
      "$src" > "$out"
  '';
in
{
  # Follow terminal appearance with the host's theme pair. Explicit CLI theme
  # flags take precedence. With CAPTURE_PROMPTS set, run behind mitmproxy that
  # logs every request/response to $XDG_STATE_HOME/prompt-capture/pi.jsonl.
  home.file.".local/bin/pi" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      ${lib.optionalString config.dotfiles.privateEnabled ''
        export PI_AGENT_BUS_URL="''${PI_AGENT_BUS_URL-http://terminus:7420}"
        export PI_AGENT_BUS_CONTROL="''${PI_AGENT_BUS_CONTROL-1}"
        export PI_AGENT_BUS_OPERATOR_NOTICES="''${PI_AGENT_BUS_OPERATOR_NOTICES-1}"
        export PI_AGENT_BUS_OPERATOR_READ="''${PI_AGENT_BUS_OPERATOR_READ-1}"
        export PI_AGENT_BUS_OPERATOR_HISTORY="''${PI_AGENT_BUS_OPERATOR_HISTORY-1}"
        bus_enabled=${if config.programs.pi-agent-bus.enable then "1" else "0"}
        bus_offline="''${PI_OFFLINE:-}"
        if [[ "''${bus_offline,,}" =~ ^[[:space:]]*(1|true|yes)[[:space:]]*$ ]]; then
          bus_enabled=0
        fi
        for arg in "$@"; do
          case "$arg" in --offline) bus_enabled=0 ;; esac
        done
        if [ "$bus_enabled" = "1" ] && [ "''${PI_AGENT_BUS_ENABLED:-}" != "0" ] \
          && [ -z "''${PI_AGENT_BUS_TOKEN+x}" ]; then
          # A missing/unavailable secret must not abort Pi under strict shell mode.
          if PI_AGENT_BUS_TOKEN="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.PI_AGENT_BUS_TOKEN.path} 2>/dev/null)"; then
            export PI_AGENT_BUS_TOKEN
          else
            unset PI_AGENT_BUS_TOKEN
          fi
        fi
        unset bus_enabled bus_offline

        if [ -z "''${TYPESAFE_API_KEY+x}" ]; then
          if TYPESAFE_API_KEY="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.TYPESAFE_API_KEY.path} 2>/dev/null)"; then
            export TYPESAFE_API_KEY
          else
            unset TYPESAFE_API_KEY
          fi
        fi
      ''}

      if [ -n "''${CAPTURE_PROMPTS:-}" ] && [ "''${CAPTURE_PROMPTS:-}" != "0" ]; then
        # Accept only authority spellings unchanged by WHATWG URL parsing.
        # Other forms disable bus participation for capture, never rewrite the
        # caller's URL. Empty URLs use the client's effective default.
        if ! bus_host="$(${piConfig}/bin/pi-capture-bus-host 2>/dev/null)"; then
          bus_host=""
        fi
        if [ -n "$bus_host" ]; then
          export NO_PROXY="''${NO_PROXY:+$NO_PROXY,}''${no_proxy:+$no_proxy,}$bus_host"
          export no_proxy="$NO_PROXY"
        else
          # Parser errors or unsupported host forms must not expose bus traffic.
          export PI_AGENT_BUS_ENABLED=0
        fi
        unset bus_host
        exec ${promptCapture}/bin/prompt-capture pi -- ${pkgs.pi-coding-agent}/bin/pi ${piThemeArgs} "$@"
      fi

      exec ${pkgs.pi-coding-agent}/bin/pi ${piThemeArgs} "$@"
    '';
  };

  # Keep credentials and user settings writable. Fill missing declared defaults
  # only; /settings remains the owner of keys that already exist.
  # Built-in catalogs stay intact; these keys only make the models available.
  # OpenCode Zen is omitted on purpose: its Astra entry looks like ChatGPT
  # subscription Astra and 401s with this account.
  home.file.".pi/agent/models.json" = lib.mkIf config.dotfiles.privateEnabled {
    source = piModelsJson;
  };
  # OpenCode console-disabled ids, and known non-4.7 Grok ids, stay out of
  # /model for Go and xAI. The id list is static, so a later model still appears.
  home.file.".pi/agent/hidden-models.json".source = ../config/pi/hidden-models.json;
  home.file.".pi/agent/extensions/hidden-models.ts".source = ../config/pi/extensions/hidden-models.ts;
  home.file.".pi/agent/extensions/pi-qwen.ts".source = ../config/local-llm/pi-qwen.ts;

  home.file.".pi/agent/extensions/pi-voice.ts" = lib.mkIf voiceFeatures.stt {
    source = ../config/pi/extensions/pi-voice.ts;
  };
  home.file.".pi/agent/extensions/mcp".source = "${mcpAdapter}/lib/node_modules/pi-mcp-adapter";
  # Keep agent state reporting in sync with the pinned Herdr source tag.
  # The installed package is a release binary and has no repository source.
  home.file.".pi/agent/extensions/herdr-agent-state.ts".source =
    "${pkgs.herdr-source}/src/integration/assets/pi/herdr-agent-state.ts";
  home.file.".pi/agent/extensions/herdr-ui.ts".source = ../config/pi/extensions/herdr-ui.ts;
  home.file.".pi/agent/extensions/herdr-model.ts".source = ../config/pi/extensions/herdr-model.ts;
  home.file.".pi/agent/extensions/context-window.ts".source =
    ../config/pi/extensions/context-window.ts;
  home.file.".pi/agent/extensions/usage.ts".source = ../config/pi/extensions/usage.ts;
  home.file.".pi/agent/extensions/skill-catalog".source = ../config/pi/extensions/skill-catalog;
  # Observational memory with Jev. Package and home default off; /om on
  # enables a session. Not a chat model: System One is called directly.
  home.file.".pi/agent/extensions/pi-observational-memory-jev".source =
    ../config/pi/extensions/pi-observational-memory-jev;
  home.file.".pi/agent/extensions/reading-policy.ts".source =
    ../config/pi/extensions/reading-policy.ts;
  home.file.".pi/agent/extensions/orchestrator-addendum.ts".source =
    ../config/pi/extensions/orchestrator-addendum.ts;
  home.file.".pi/agent/ORCHESTRATOR.md".source = ../config/llm/ORCHESTRATOR.md;
  home.file.".pi/agent/extensions/goal.ts".text = ''
    export { default } from "${../config/pi/goal}/index.ts";
  '';
  home.file.".pi/agent/extensions/question.ts".source = "${piExtensions}/question.ts";
  # Validation errors have details = {}, not a todo state snapshot. The pinned
  # example crashes when rendering these errors or restoring their session.
  home.file.".pi/agent/extensions/todo.ts".source = pkgs.runCommand "pi-todo.ts" { } ''
    cp ${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/todo.ts "$out"
    chmod u+w "$out"
    substituteInPlace "$out" --replace-fail \
      'if (!details) {' \
      'if (!details || !["list", "add", "toggle", "clear"].includes(details.action)) {' \
      --replace-fail \
      'if (details) {' \
      'if (details && Array.isArray(details.todos) && Number.isInteger(details.nextId)) {'
  '';
  # Subagent delegation auto-discovered by Pi. Agent frontmatter pins model
  # and thinking for each role.
  # Adapted pinned upstream example: interactive herdr teams and persona skills.
  home.file.".pi/agent/extensions/subagent".source = "${piExtensions}/subagent";
  # Pinned upstream fuzzy history overlay. Enter restores without submitting.
  # History/index/settings follow getAgentDir().
  home.file.".pi/agent/extensions/prompt-history".source = promptHistory;
  home.file.".pi/agent/agents/scout.md".source = ../config/pi/agents/scout.md;
  home.file.".pi/agent/agents/planner.md".source = ../config/pi/agents/planner.md;
  home.file.".pi/agent/agents/reviewer.md".source = ../config/pi/agents/reviewer.md;
  home.file.".pi/agent/agents/worker.md".source = ../config/pi/agents/worker.md;
  home.file.".pi/agent/agents/architect.md".source = ../config/pi/agents/architect.md;
  home.file.".pi/agent/agents/builder.md".source = ../config/pi/agents/builder.md;
  home.file.".pi/agent/agents/sceptic.md".source = ../config/pi/agents/sceptic.md;
  home.file.".pi/agent/agents/test-engineer.md".source = ../config/pi/agents/test-engineer.md;
  home.file.".pi/agent/agents/security-reviewer.md".source = ../config/pi/agents/security-reviewer.md;
  home.file.".pi/agent/agents/domain-specialist.md".source = ../config/pi/agents/domain-specialist.md;
  home.file.".pi/agent/agents/interface-designer.md".source =
    ../config/pi/agents/interface-designer.md;
  home.file.".pi/agent/agents/dashboard-designer.md".source =
    ../config/pi/agents/dashboard-designer.md;
  home.file.".pi/agent/agents/communication-designer.md".source =
    ../config/pi/agents/communication-designer.md;
  home.file.".pi/agent/prompts/implement.md".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/subagent/prompts/implement.md";
  home.file.".pi/agent/prompts/scout-and-plan.md".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/subagent/prompts/scout-and-plan.md";
  home.file.".pi/agent/prompts/implement-and-review.md".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/subagent/prompts/implement-and-review.md";
  home.file.".pi/agent/prompts/plan-work.md".source = ../config/pi/prompts/plan-work.md;
  home.file.".pi/agent/prompts/review.md".source = ../config/pi/prompts/review.md;
  home.file.".pi/agent/prompts/design.md".source = ../config/pi/prompts/design.md;

  # Credentials remain in the shared runtime MCP wrappers.
  home.file.".config/mcp/mcp.json".text = builtins.toJSON {
    mcpServers = llmMcps.piServers;
    settings = {
      hostConfigDiscovery = "off";
      directTools = false;
      namespaceProxyTools = false;
      scriptMode = false;
      idleTimeout = 10;
      mcpFooterStatus = "compact";
    };
  };

  home.activation.piSettingsDefaults = lib.hm.dag.entryAfter [ "sops-nix" ] ''
    ${piConfig}/bin/pi-merge-settings \
      ${../config/pi/settings-defaults.json} \
      "$HOME/.pi/agent/settings.json"
    ${piConfig}/bin/pi-merge-settings \
      ${../config/pi/keybindings-defaults.json} \
      "$HOME/.pi/agent/keybindings.json"
    ${lib.optionalString config.dotfiles.privateEnabled ''
      ${piConfig}/bin/pi-merge-auth \
        "$HOME/.pi/agent/auth.json" \
        --drop openai \
        --drop opencode \
        --oauth-provider openai-codex \
        --refresh-file ${lib.escapeShellArg config.sops.secrets.PI_OPENAI_CODEX_REFRESH.path} \
        --account-file ${lib.escapeShellArg config.sops.secrets.PI_OPENAI_CODEX_ACCOUNT_ID.path}
    ''}
  '';
}
