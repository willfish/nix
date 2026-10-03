{
  config,
  lib,
  pkgs,
  hostName,
  readSopsSecret,
  ...
}:
let
  llmMcps = import ./llm-mcps.nix { inherit config; };
  piModels = builtins.fromJSON (builtins.readFile ../config/pi/models.json);
  adapt = ../config/opencode/adapt-markdown.py;
  qwenBaseUrl =
    host:
    if hostName == host then "http://127.0.0.1:8081/v1" else "http://${host}.taile09696.ts.net:8081/v1";
  providerModel = model: {
    inherit (model) name;
    limit = {
      context = model.contextWindow;
      output = model.maxTokens;
    };
  };
  localProvider = name: baseUrl: envName: models: {
    npm = "@ai-sdk/openai-compatible";
    inherit name;
    options = {
      baseURL = baseUrl;
      apiKey = "{env:${envName}}";
    };
    models = builtins.listToAttrs (
      map (model: {
        name = model.id;
        value = providerModel model;
      }) models
    );
  };
  mcpCommand = name: "${config.home.homeDirectory}/.local/bin/${name}";
  mcpServers = builtins.listToAttrs (
    map (server: {
      inherit (server) name;
      value =
        if server ? url then
          {
            type = "remote";
            inherit (server) url;
            enabled = true;
            oauth = false;
            timeout = (server.toolTimeout or 120) * 1000;
          }
        else
          {
            type = "local";
            command = [ (mcpCommand server.wrapper) ];
            enabled = true;
            timeout = (server.toolTimeout or 120) * 1000;
          };
    }) llmMcps.servers
  );
  adapted =
    kind: source:
    pkgs.runCommand "opencode-${kind}" { } ''
      mkdir -p "$out"
      ${pkgs.python3}/bin/python3 ${adapt} --kind ${kind} ${source} "$out"
    '';
  upstreamPrompts = "${pkgs.pi-coding-agent}/libexec/pi/examples/extensions/subagent/prompts";
  commands = pkgs.runCommand "opencode-commands" { } ''
    mkdir -p "$out"
    ${pkgs.python3}/bin/python3 ${adapt} --kind command ${../config/pi/prompts} "$out"
    ${pkgs.python3}/bin/python3 ${adapt} --kind command ${upstreamPrompts} "$out"
  '';
  busEnabled =
    config.dotfiles.privateEnabled && ((config.programs.pi-agent-bus or { }).enable or false);
  busClient =
    pkgs.runCommand "opencode-bus-client"
      {
        src = config.programs.pi-agent-bus.package;
      }
      ''
        mkdir -p "$out/extension" "$out/node_modules/@earendil-works/pi-tui"
        cp -a "$src/extension/." "$out/extension/"
        cp ${../config/opencode/pi-tui-stub.js} "$out/node_modules/@earendil-works/pi-tui/index.js"
        printf '%s\n' '{"name":"@earendil-works/pi-tui","type":"module","main":"index.js"}' \
          > "$out/node_modules/@earendil-works/pi-tui/package.json"
      '';
  busPlugin = lib.replaceStrings [ "@BUS_CLIENT@" ] [ "${busClient}" ] (
    builtins.readFile ../config/opencode/agent-bus.ts
  );
  sopsExport = name: ''
    if [ -z "''${${name}+x}" ]; then
      if ${name}="$(${readSopsSecret}/bin/read-sops-secret ${
        lib.escapeShellArg config.sops.secrets.${name}.path
      } 2>/dev/null)"; then
        export ${name}
      else
        unset ${name}
      fi
    fi
  '';
in
{
  home.packages = [ pkgs.opencode ];

  home.file = {
    ".local/bin/opencode" = {
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
          export PI_AGENT_BUS_AUTO_LABEL="''${PI_AGENT_BUS_AUTO_LABEL-0}"
          ${sopsExport "PI_AGENT_BUS_TOKEN"}
          ${sopsExport "OPENCODE_GO_KEY"}
          ${sopsExport "LOCAL_LLM_RELAY_API_KEY"}
          ${sopsExport "LOCAL_LLM_ANDROMEDA_API_KEY"}
        ''}

        exec ${pkgs.opencode}/bin/opencode "$@"
      '';
    };
    ".config/opencode/opencode.json".text = builtins.toJSON (
      {
        "$schema" = "https://opencode.ai/config.json";
        mcp = mcpServers;
      }
      // lib.optionalAttrs config.dotfiles.privateEnabled {
        provider = {
          "opencode-go" = {
            options.apiKey = "{env:OPENCODE_GO_KEY}";
          };
          relay =
            localProvider "Relay" (qwenBaseUrl "relay") "LOCAL_LLM_RELAY_API_KEY"
              piModels.providers.relay.models;
          andromeda =
            localProvider "Andromeda" (qwenBaseUrl "andromeda") "LOCAL_LLM_ANDROMEDA_API_KEY"
              piModels.providers.andromeda.models;
        };
      }
    );
    ".config/opencode/agents".source = adapted "agent" ../config/pi/agents;
    ".config/opencode/commands".source = commands;
    ".config/opencode/ORCHESTRATOR.md".source = ../config/llm/ORCHESTRATOR.md;
  }
  // lib.optionalAttrs busEnabled {
    ".config/opencode/plugins/agent-bus.ts".source = pkgs.writeText "agent-bus.ts" busPlugin;
  };
}
