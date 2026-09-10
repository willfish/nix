{
  config,
  lib,
  pkgs,
  ...
}:
let
  llmMcps = import ./llm-mcps.nix { inherit config lib; };
  mcpAdapter = pkgs.callPackage ./mcp-packages/pi-mcp-adapter.nix { };
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
  home.file.".pi/agent/models.json".source = ../config/pi/models.json;

  home.file.".pi/agent/extensions/mcp".source = "${mcpAdapter}/lib/node_modules/pi-mcp-adapter";
  # Keep agent state reporting in sync with the pinned Herdr package.
  home.file.".pi/agent/extensions/herdr-agent-state.ts".source =
    "${pkgs.herdr.src}/src/integration/assets/pi/herdr-agent-state.ts";
  home.file.".pi/agent/extensions/herdr-ui.js".source = ../config/pi/extensions/herdr-ui.js;
  home.file.".pi/agent/extensions/herdr-model.js".source = ../config/pi/extensions/herdr-model.js;
  home.file.".pi/agent/extensions/context-window.js".source =
    ../config/pi/extensions/context-window.js;
  home.file.".pi/agent/extensions/usage.js".source = ../config/pi/extensions/usage.js;
  # Use the example shipped with the pinned Pi runtime and its host API.
  home.file.".pi/agent/extensions/todo.ts".source =
    "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/todo.ts";
  home.file.".pi/agent/prompts/plan-work.md".source = ../config/pi/prompts/plan-work.md;
  home.file.".pi/agent/prompts/review.md".source = ../config/pi/prompts/review.md;

  # The adapter reads this shared path even with PI_CODING_AGENT_DIR set by
  # qwen-pi. Credentials remain in the same runtime wrappers as Codex/Grok.
  home.file.".config/mcp/mcp.json".text = builtins.toJSON {
    mcpServers = llmMcps.piServers;
    settings = {
      hostConfigDiscovery = "off";
      directTools = false;
      scriptMode = false;
      idleTimeout = 10;
      mcpFooterStatus = "compact";
    };
  };
}
