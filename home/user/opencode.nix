{
  config,
  lib,
  pkgs,
  ...
}:
let
  defaultModel = "openrouter/z-ai/glm-5.2";
  llmMcps = import ./llm-mcps.nix { inherit config lib; };
  mcpCommand = name: "${config.home.homeDirectory}/.local/bin/${name}";
  localMcp = name: {
    type = "local";
    command = [
      (mcpCommand name)
    ];
    enabled = true;
    timeout = 120000;
  };

  opencodeConfig = (pkgs.formats.json { }).generate "opencode.json" {
    "$schema" = "https://opencode.ai/config.json";

    model = defaultModel;
    small_model = "openrouter/openai/gpt-4o-mini";
    autoupdate = false;
    share = "manual";

    instructions = [
      "~/.config/opencode/AGENTS.md"
    ];

    enabled_providers = [
      "openrouter"
      "opencode"
    ];

    provider = {
      opencode = {
        env = [ "OPENCODE_API_KEY" ];
        options = {
          timeout = 600000;
        };
      };
      openrouter = {
        env = [ "OPENROUTER_API_KEY" ];
        options = {
          timeout = 600000;
        };
        models = {
          "x-ai/grok-4.3" = {
            name = "Grok 4.3";
            reasoning = true;
            tool_call = true;
            limit = {
              context = 1000000;
              output = 128000;
            };
          };
          "z-ai/glm-5.2" = {
            name = "GLM 5.2";
            reasoning = true;
            tool_call = true;
            limit = {
              context = 1048576;
              output = 131072;
            };
          };
          "moonshotai/kimi-k2.7-code" = {
            name = "Kimi K2.7 Code";
            reasoning = true;
            tool_call = true;
            limit = {
              context = 262144;
              output = 262144;
            };
          };
          "deepseek/deepseek-v4-pro" = {
            name = "DeepSeek V4 Pro";
            reasoning = true;
            tool_call = true;
            limit = {
              context = 1048576;
              output = 128000;
            };
          };
          "minimax/minimax-m3" = {
            name = "MiniMax M3";
            reasoning = true;
            tool_call = true;
            limit = {
              context = 1048576;
              output = 128000;
            };
          };
          "qwen/qwen3-coder" = {
            name = "Qwen3 Coder";
            reasoning = true;
            tool_call = true;
            limit = {
              context = 1048576;
              output = 128000;
            };
          };
        };
      };
    };

    mcp = llmMcps.opencodeServers localMcp;

    permission = "allow";
  };

in
{
  home.packages = [
    pkgs.opencode
  ];

  home.file.".local/bin/opencode" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      if [ -z "''${OPENROUTER_API_KEY:-}" ]; then
        openrouter_key_file="$HOME/.config/sops-nix/secrets/OPENROUTER_API_KEY"
        if [ -r "$openrouter_key_file" ]; then
          OPENROUTER_API_KEY="$(<"$openrouter_key_file")"
          OPENROUTER_API_KEY="''${OPENROUTER_API_KEY%\"}"
          OPENROUTER_API_KEY="''${OPENROUTER_API_KEY#\"}"
          export OPENROUTER_API_KEY
        fi
      fi

      if [ -z "''${OPENCODE_API_KEY:-}" ]; then
        opencode_key_file="$HOME/.config/sops-nix/secrets/OPENCODE_API_KEY"
        if [ -r "$opencode_key_file" ]; then
          OPENCODE_API_KEY="$(<"$opencode_key_file")"
          OPENCODE_API_KEY="''${OPENCODE_API_KEY%\"}"
          OPENCODE_API_KEY="''${OPENCODE_API_KEY#\"}"
          if [ -n "$OPENCODE_API_KEY" ] && [ "$OPENCODE_API_KEY" != "__UNSET__" ]; then
            export OPENCODE_API_KEY
          fi
        fi
      fi

      exec ${pkgs.opencode}/bin/opencode "$@"
    '';
  };

  home.file.".config/opencode/opencode.json" = {
    source = opencodeConfig;
    force = true;
  };
}
