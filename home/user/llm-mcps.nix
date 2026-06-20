{ config }:
let
  mcpCommand = name: "${config.home.homeDirectory}/.local/bin/${name}";
  serverDefaults = {
    toolTimeout = 120;
  };
in
rec {
  # The catalogue drives both package/wrapper installation and Pi registration.
  servers = builtins.filter (server: server.enabled) (
    [
      {
        name = "jira";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-jira";
      }
      {
        name = "github";
        enabled = true;
        wrapper = "mcp-github";
      }
      {
        name = "browser";
        enabled = config.dotfiles.capabilities.playwright;
        wrapper = "mcp-agent-browser";
      }
      {
        name = "browser-playwright";
        enabled = config.dotfiles.capabilities.playwright;
        wrapper = "mcp-brave";
      }
      {
        name = "terraform";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-terraform";
      }
      {
        name = "nixos";
        enabled = true;
        wrapper = "mcp-nixos";
      }
      {
        name = "slack";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-slack";
      }
      {
        name = "telegram";
        enabled = config.dotfiles.capabilities.telegram;
        wrapper = "mcp-telegram";
      }
    ]
    ++ map (server: server // { private = true; }) config.privateConfig.mcpServers
  );

  isHttp = server: server ? url;
  commandFor = server: mcpCommand server.wrapper;

  piServers = builtins.listToAttrs (
    map (server: {
      inherit (server) name;
      value = (if isHttp server then { inherit (server) url; } else { command = commandFor server; }) // {
        lifecycle = "lazy";
        directTools = false;
        protocolVersion = "legacy";
        requestTimeoutMs = serverDefaults.toolTimeout * 1000;
      };
    }) servers
  );
}
