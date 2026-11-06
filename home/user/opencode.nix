{
  config,
  lib,
  pkgs,
  hostName,
  readSopsSecret,
  ...
}:
let
  llmMcps = import ./llm-mcps.nix { inherit config pkgs; };
  piModels = builtins.fromJSON (builtins.readFile ../config/pi/models.json);
  adapt = lib.getExe (import ../../programs/opencode-adapt { pkgs = pkgs.buildPackages; });
  qwenBaseUrl =
    host:
    if hostName == host then "http://127.0.0.1:8081/v1" else "http://${host}.taile09696.ts.net:8081/v1";
  providerModel = model: {
    inherit (model) name;
    capabilities.tools = true;
    limit = {
      context = model.contextWindow;
      output = model.maxTokens;
    };
  };
  localProvider = name: baseUrl: envName: models: {
    package = "@opencode/ai/providers/openai-compatible";
    inherit name;
    settings = {
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
            description = server.description or server.name;
            inherit (server) url;
            codemode = true;
            lazy = true;
            oauth = false;
            timeout.execution = (server.toolTimeout or 120) * 1000;
          }
        else
          {
            type = "local";
            description = server.description or server.name;
            command = [ (mcpCommand server.wrapper) ];
            codemode = true;
            lazy = true;
            timeout.execution = (server.toolTimeout or 120) * 1000;
          };
    }) llmMcps.servers
  );
  agentNames = map (name: lib.removeSuffix ".md" name) (
    builtins.attrNames (
      lib.filterAttrs (name: type: type == "regular" && lib.hasSuffix ".md" name) (
        builtins.readDir ../config/pi/agents
      )
    )
  );
  agentField =
    name: field:
    let
      line = lib.findFirst (lib.hasPrefix "${field}: ") "" (
        lib.splitString "\n" (builtins.readFile (../config/pi/agents + "/${name}.md"))
      );
    in
    lib.removePrefix "${field}: " line;
  agentModel =
    name:
    let
      model = lib.replaceStrings [ "openai-codex/" ] [ "openai/" ] (agentField name "model");
      thinking = agentField name "thinking";
    in
    model + lib.optionalString (thinking != "" && thinking != "off") "#${thinking}";
  agentSkills = builtins.listToAttrs (
    map (name: {
      inherit name;
      value = lib.filter (skill: skill != "") (
        map lib.trim (
          lib.splitString "," (lib.removeSuffix "]" (lib.removePrefix "[" (agentField name "skills")))
        )
      );
    }) agentNames
  );
  agents = pkgs.runCommand "opencode-v2-agents" { } ''
    mkdir -p "$out"
    ${adapt} --kind agent ${../config/pi/agents} "$out"
    ${lib.concatMapStringsSep "\n" (name: ''
      ${lib.optionalString (agentField name "model" != "") ''
        substituteInPlace "$out/${name}.md" --replace-fail \
          ${lib.escapeShellArg "model: ${agentField name "model"}"} \
          ${lib.escapeShellArg "model: ${agentModel name}"}
      ''}
      ${lib.optionalString (agentSkills.${name} != [ ]) ''
        substituteInPlace "$out/${name}.md" --replace-fail \
          ${lib.escapeShellArg "Load these skills when the task reaches their trigger: ${lib.concatStringsSep ", " agentSkills.${name}}.\n"} \
          ""
      ''}
    '') agentNames}
    for file in "$out/"*.md; do
      substituteInPlace "$file" \
        --replace-quiet 'ask_coordinator' 'the native question tool' \
        --replace-quiet ', with requiresUser.' ', using the native question tool for human decisions.'
    done
  '';
  commands = pkgs.runCommand "opencode-commands" { } ''
    mkdir -p "$out"
    ${adapt} --kind command ${../config/pi/prompts} "$out"
    # Use OpenCode's built-in review command.
    rm "$out/review.md"
    cp ${../config/opencode/commands}/*.md "$out/"
    substituteInPlace "$out/plan-work.md" --replace-fail \
      'Use the todo tool for' 'Use the native todowrite tool for'
  '';
  orchestratorRules = lib.concatMapStringsSep "\n" (section: "## ${lib.trim section}\n") (
    lib.filter (lib.hasPrefix "Design routing\n") (
      lib.splitString "## " (builtins.readFile ../config/llm/ORCHESTRATOR.md)
    )
  );
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
  harness = pkgs.runCommand "opencode-workflow" { } ''
    mkdir -p "$out"
    cp -r ${../config/opencode/harness}/. "$out/"
    substituteInPlace "$out/speech-queue.ts" --replace-fail \
      'const FLOCK = "flock";' 'const FLOCK = "${pkgs.flock}/bin/flock";'
    cp ${../config/pi/extensions/skill-catalog/catalog.ts} "$out/skill-search.ts"
    cp ${
      if busEnabled then
        pkgs.writeText "bus-source.ts" busPlugin
      else
        pkgs.writeText "bus-source.ts" "export default async function () { return null; }"
    } "$out/bus-source.ts"
    cp ${pkgs.writeText "role-skills.json" (builtins.toJSON agentSkills)} "$out/role-skills.json"
  '';
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
  home.packages = [
    pkgs.opencode2
    pkgs.claude-code
  ];

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
          ${sopsExport "ELEVENLABS_API_KEY"}
          ${sopsExport "LOCAL_LLM_RELAY_API_KEY"}
          ${sopsExport "LOCAL_LLM_ANDROMEDA_API_KEY"}
        ''}

        exec ${pkgs.opencode2}/bin/opencode2 "$@"
      '';
    };
    ".config/opencode/cli.json".text = builtins.toJSON {
      "$schema" = "https://opencode.ai/v2/cli.json";
      session.sidebar = "hide";
      keybinds = {
        "app.clear" = "none";
        "model.list" = "ctrl+l,<leader>m";
        "prompt.editor" = "ctrl+g,<leader>e";
        "session.first" = "home";
      };
    };
    ".config/opencode/opencode.json".text = builtins.toJSON (
      {
        "$schema" = "https://opencode.ai/config.json";
        mcp.servers = mcpServers;
        autoupdate = false;
        experimental.todo = true;
        permissions = [
          {
            action = "*";
            resource = "*";
            effect = "allow";
          }
        ];
        plugins = [
          {
            package = "${harness}";
            options.busEnabled = busEnabled;
          }
        ];
        agents = builtins.listToAttrs (
          map (name: {
            inherit name;
            value.permissions = [
              {
                action = "subagent";
                resource = "*";
                effect = "deny";
              }
            ]
            ++
              lib.optional (agentField name "tools" != "" && !(lib.hasInfix "edit" (agentField name "tools")))
                {
                  action = "edit";
                  resource = "*";
                  effect = "deny";
                };
          }) agentNames
        );
      }
      // {
        providers =
          lib.optionalAttrs config.dotfiles.privateEnabled {
            "opencode-go" = {
              settings.apiKey = "{env:OPENCODE_GO_KEY}";
            };
            relay =
              localProvider "Relay" (qwenBaseUrl "relay") "LOCAL_LLM_RELAY_API_KEY"
                piModels.providers.relay.models;
            andromeda =
              localProvider "Andromeda" (qwenBaseUrl "andromeda") "LOCAL_LLM_ANDROMEDA_API_KEY"
                piModels.providers.andromeda.models;
          }
          // lib.optionalAttrs (hostName == "andromeda") {
            hex-rewrite = {
              package = "@opencode/ai/providers/openai-compatible";
              name = "Hex rewrite (CPU)";
              settings.baseURL = "http://127.0.0.1:18083/v1";
              models."qwen3-4b-instruct-2507" = {
                name = "Qwen3 4B Instruct (CPU)";
                capabilities.tools = false;
                limit = {
                  context = 8192;
                  output = 1024;
                };
              };
            };
          };
      }
    );
    ".config/opencode/agents".source = agents;
    ".config/opencode/commands".source = commands;
    ".config/opencode/ORCHESTRATOR.md".text = orchestratorRules;
  };
}
