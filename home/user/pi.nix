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
  # Preserve the source document's layout; builtins.toJSON would minify it.
  piModelsJson = pkgs.runCommand "pi-models.json" { src = ../config/pi/models.json; } ''
    ${pkgs.jq}/bin/jq \
      --arg go ${lib.escapeShellArg (sopsApiKey "OPENCODE_GO_KEY")} \
      --arg openrouter ${lib.escapeShellArg (sopsApiKey "OPENROUTER_API_KEY")} \
      '.providers["opencode-go"].apiKey = $go | .providers.openrouter.apiKey = $openrouter' \
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

      if [ -n "''${CAPTURE_PROMPTS:-}" ] && [ "''${CAPTURE_PROMPTS:-}" != "0" ]; then
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
  home.file.".pi/agent/extensions/goal.ts".source = ../config/pi/extensions/goal.ts;
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
