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
  voiceFeatures = import ./voice-supported.nix { inherit pkgs hostName; };
  llmMcps = import ./llm-mcps.nix { inherit config; };
  mcpAdapter = pkgs.callPackage ./mcp-packages/pi-mcp-adapter.nix { };
  promptHistory = pkgs.callPackage ./pi-packages/prompt-history.nix { };
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
      --arg openrouter ${lib.escapeShellArg (sopsApiKey "OPENROUTER_API_KEY")} \
      --arg relay ${lib.escapeShellArg (sopsApiKey "LOCAL_LLM_RELAY_API_KEY")} \
      --arg andromeda ${lib.escapeShellArg (sopsApiKey "LOCAL_LLM_ANDROMEDA_API_KEY")} \
      --arg relayUrl ${lib.escapeShellArg (qwenBaseUrl "relay")} \
      --arg andromedaUrl ${lib.escapeShellArg (qwenBaseUrl "andromeda")} \
      '.providers["opencode-go"].apiKey = $go
       | .providers.openrouter.apiKey = $openrouter
       | .providers.relay.apiKey = $relay
       | .providers.andromeda.apiKey = $andromeda
       | .providers.relay.baseUrl = $relayUrl
       | .providers.andromeda.baseUrl = $andromedaUrl' \
      "$src" > "$out"
  '';
in
{
  programs.pi-agent-bus.enable = lib.mkDefault true;

  # Follow terminal appearance with the host's theme pair. Explicit CLI theme
  # flags take precedence. With CAPTURE_PROMPTS set, run behind mitmproxy that
  # logs every request/response to $XDG_STATE_HOME/prompt-capture/pi.jsonl.
  home.file.".local/bin/pi" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      export PI_AGENT_BUS_URL="''${PI_AGENT_BUS_URL-http://terminus:7420}"
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

      if [ -n "''${CAPTURE_PROMPTS:-}" ] && [ "''${CAPTURE_PROMPTS:-}" != "0" ]; then
        # Accept only authority spellings unchanged by WHATWG URL parsing.
        # Other forms disable bus participation for capture, never rewrite the
        # caller's URL. Empty URLs use the client's effective default.
        if ! bus_host="$(${pkgs.python3}/bin/python3 -c '
      import ipaddress, os, re
      raw = os.environ.get("PI_AGENT_BUS_URL") or "http://terminus:7420"
      match = re.fullmatch(r"https?://([A-Za-z0-9.-]+)(?::[0-9]+)?(?:/[^\s\\?#]*)?", raw)
      host = match[1].lower() if match else ""
      labels = host.split(".")
      if not all(re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label) for label in labels):
          host = ""
      elif re.fullmatch(r"[0-9]+|0x[0-9a-f]*", labels[-1]):
          try:
              if str(ipaddress.IPv4Address(host)) != host:
                  host = ""
          except ValueError:
              host = ""
      print(host)
      ' 2>/dev/null)"; then
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
  # qwen-pi has a separate local profile and does not use these models.
  # Built-in catalogs stay intact; these keys only make the models available.
  # OpenCode Zen is omitted on purpose: its Astra entry looks like ChatGPT
  # subscription Astra and 401s with this account.
  home.file.".pi/agent/models.json".source = piModelsJson;
  home.file.".pi/agent/extensions/pi-qwen.ts".source = ../config/local-llm/pi-qwen.ts;

  home.file.".pi/agent/extensions/pi-voice.ts" = lib.mkIf voiceFeatures.stt {
    source = ../config/pi/extensions/pi-voice.ts;
  };
  home.file.".pi/agent/extensions/mcp".source = "${mcpAdapter}/lib/node_modules/pi-mcp-adapter";
  # Keep agent state reporting in sync with the pinned Herdr package.
  home.file.".pi/agent/extensions/herdr-agent-state.ts".source =
    "${pkgs.herdr.src}/src/integration/assets/pi/herdr-agent-state.ts";
  home.file.".pi/agent/extensions/herdr-ui.ts".source = ../config/pi/extensions/herdr-ui.ts;
  home.file.".pi/agent/extensions/herdr-model.ts".source = ../config/pi/extensions/herdr-model.ts;
  home.file.".pi/agent/extensions/context-window.ts".source =
    ../config/pi/extensions/context-window.ts;
  home.file.".pi/agent/extensions/usage.ts".source = ../config/pi/extensions/usage.ts;
  home.file.".pi/agent/extensions/skill-catalog".source = ../config/pi/extensions/skill-catalog;
  home.file.".pi/agent/extensions/reading-policy.ts".source =
    ../config/pi/extensions/reading-policy.ts;
  home.file.".pi/agent/extensions/orchestrator-addendum.ts".source =
    ../config/pi/extensions/orchestrator-addendum.ts;
  home.file.".pi/agent/ORCHESTRATOR.md".source = ../config/llm/ORCHESTRATOR.md;
  home.file.".pi/agent/extensions/goal.ts".source = ../config/pi/extensions/goal.ts;
  home.file.".pi/agent/extensions/question.ts".source = ../config/pi/extensions/question.ts;
  # Use the example shipped with the pinned Pi runtime and its host API.
  home.file.".pi/agent/extensions/todo.ts".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/todo.ts";
  # Subagent delegation for the standard profile. Auto-discovered by pi; the
  # qwen-pi launcher passes --no-extensions plus an explicit list that omits
  # it, so the memory-constrained local Qwen profile never spawns subagent
  # processes. Agent definitions pin no model, so subagents run on whatever
  # Grok/OpenAI model the session is using.
  # Adapted pinned upstream example: interactive herdr teams and persona skills.
  home.file.".pi/agent/extensions/subagent".source = ../config/pi/extensions/subagent;
  # Pinned upstream fuzzy history overlay. Enter restores without submitting.
  # Both launchers share code, but history/index/settings follow getAgentDir().
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
  home.file.".pi/agent/prompts/implement.md".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/subagent/prompts/implement.md";
  home.file.".pi/agent/prompts/scout-and-plan.md".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/subagent/prompts/scout-and-plan.md";
  home.file.".pi/agent/prompts/implement-and-review.md".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/subagent/prompts/implement-and-review.md";
  home.file.".pi/agent/prompts/plan-work.md".source = ../config/pi/prompts/plan-work.md;
  home.file.".pi/agent/prompts/review.md".source = ../config/pi/prompts/review.md;

  # The adapter reads this shared path even with PI_CODING_AGENT_DIR set by
  # qwen-pi. Credentials remain in the shared runtime MCP wrappers.
  home.file.".config/mcp/mcp.json".text = builtins.toJSON {
    mcpServers = llmMcps.piServers;
    settings = {
      hostConfigDiscovery = "off";
      directTools = false;
      namespaceTools = false;
      scriptMode = false;
      idleTimeout = 10;
      mcpFooterStatus = "compact";
    };
  };

  home.activation.piSettingsDefaults = lib.hm.dag.entryAfter [ "sops-nix" ] ''
    ${pkgs.python3}/bin/python3 ${../config/pi/merge-settings.py} \
      ${../config/pi/settings-defaults.json} \
      "$HOME/.pi/agent/settings.json"
    ${pkgs.python3}/bin/python3 ${../config/pi/merge-settings.py} \
      ${../config/pi/keybindings-defaults.json} \
      "$HOME/.pi/agent/keybindings.json"
    ${pkgs.python3}/bin/python3 ${../config/pi/merge-auth.py} \
      "$HOME/.pi/agent/auth.json" \
      --drop openai \
      --drop opencode \
      --oauth-provider openai-codex \
      --refresh-file ${lib.escapeShellArg config.sops.secrets.PI_OPENAI_CODEX_REFRESH.path} \
      --account-file ${lib.escapeShellArg config.sops.secrets.PI_OPENAI_CODEX_ACCOUNT_ID.path}
  '';
}
