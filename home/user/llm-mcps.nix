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
        name = "web-search";
        enabled = true;
        wrapper = "mcp-web-search";
      }
      {
        name = "browser";
        enabled = config.dotfiles.capabilities.playwright;
        wrapper = "mcp-agent-browser";
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
        name = "filesystem";
        enabled = true;
        wrapper = "mcp-filesystem";
      }
      {
        name = "dap";
        enabled = config.dotfiles.capabilities.development;
        wrapper = "mcp-dap";
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
      {
        name = "gmail";
        enabled = config.dotfiles.capabilities.email;
        wrapper = "mcp-himalaya";
      }
      {
        name = "aws-access-portal";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-aws-access-portal";
        toolTimeout = 180;
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
        requestTimeoutMs = (server.toolTimeout or serverDefaults.toolTimeout) * 1000;
      };
    }) servers
  );
}
