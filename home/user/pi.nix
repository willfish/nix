{
  config,
  lib,
  pkgs,
  hostName,
  readSopsSecret,
  ...
}:
let
  voiceFeatures = import ./voice-supported.nix { inherit pkgs hostName; };
  llmMcps = import ./llm-mcps.nix { inherit config; };
  mcpAdapter = pkgs.callPackage ./mcp-packages/pi-mcp-adapter.nix { };
  promptHistory = pkgs.callPackage ./pi-packages/prompt-history.nix { };
  promptCapture = import ./prompt-capture.nix { inherit pkgs; };
in
{
  # Wrapper around the Home Manager pi package. Default behaviour is
  # unchanged; with CAPTURE_PROMPTS set it runs behind a local mitmproxy that
  # logs every request/response to $XDG_STATE_HOME/prompt-capture/pi.jsonl.
  home.file.".local/bin/pi" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      if [ -n "''${CAPTURE_PROMPTS:-}" ] && [ "''${CAPTURE_PROMPTS:-}" != "0" ]; then
        exec ${promptCapture}/bin/prompt-capture pi -- ${pkgs.pi-coding-agent}/bin/pi "$@"
      fi

      exec ${pkgs.pi-coding-agent}/bin/pi "$@"
    '';
  };

  # Keep credentials and user settings writable and outside Home Manager.
  # qwen-pi has a separate local profile and does not use these models.
  home.file.".pi/agent/models.json".text = builtins.toJSON (
    lib.recursiveUpdate (builtins.fromJSON (builtins.readFile ../config/pi/models.json)) {
      providers.opencode-go.apiKey = "!${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.OPENCODE_GO_KEY.path}";
    }
  );

  home.file.".pi/agent/extensions/pi-voice.js" = lib.mkIf voiceFeatures.stt {
    source = ../config/pi/extensions/pi-voice.js;
  };
  home.file.".pi/agent/extensions/mcp".source = "${mcpAdapter}/lib/node_modules/pi-mcp-adapter";
  # Keep agent state reporting in sync with the pinned Herdr package.
  home.file.".pi/agent/extensions/herdr-agent-state.ts".source =
    "${pkgs.herdr.src}/src/integration/assets/pi/herdr-agent-state.ts";
  home.file.".pi/agent/extensions/herdr-ui.js".source = ../config/pi/extensions/herdr-ui.js;
  home.file.".pi/agent/extensions/herdr-model.js".source = ../config/pi/extensions/herdr-model.js;
  home.file.".pi/agent/extensions/context-window.js".source =
    ../config/pi/extensions/context-window.js;
  home.file.".pi/agent/extensions/usage.js".source = ../config/pi/extensions/usage.js;
  home.file.".pi/agent/extensions/skill-catalog".source = ../config/pi/extensions/skill-catalog;
  home.file.".pi/agent/extensions/reading-policy.js".source =
    ../config/pi/extensions/reading-policy.js;
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
}
