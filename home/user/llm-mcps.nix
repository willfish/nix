{ config, pkgs }:
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
        description = "Issues, projects and workflows";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-jira";
      }
      {
        name = "github";
        description = "Repositories, code, issues and pull requests";
        enabled = config.dotfiles.privateEnabled;
        wrapper = "mcp-github";
      }
      {
        name = "arxiv";
        description = "Search and read research papers";
        enabled = config.dotfiles.privateEnabled;
        wrapper = "mcp-arxiv";
      }
      {
        name = "books";
        description = "Search and read the offline book and reference library on Terminus";
        enabled = config.dotfiles.privateEnabled;
        wrapper = "mcp-books";
      }
      {
        name = "books-local";
        description = "Search and read the portable offline essentials copy without network access";
        enabled =
          config.dotfiles.privateEnabled && config.dotfiles.capabilities.desktop && pkgs.stdenv.isLinux;
        wrapper = "mcp-books-local";
      }
      {
        name = "web-search";
        description = "Search the public web";
        enabled = config.dotfiles.privateEnabled;
        wrapper = "mcp-web-search";
      }
      {
        name = "browser";
        description = "Control the existing visible browser";
        enabled = config.dotfiles.capabilities.playwright;
        wrapper = "mcp-agent-browser";
      }
      {
        name = "tor-mcp";
        description = "Tor browser sessions and onion research";
        enabled =
          config.dotfiles.privateEnabled
          && config.dotfiles.capabilities.desktop
          && pkgs.stdenv.hostPlatform.system == "x86_64-linux";
        wrapper = "mcp-tor";
        toolTimeout = 360;
      }
      {
        name = "terraform";
        description = "Terraform provider and module registries";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-terraform";
      }
      {
        name = "nixos";
        description = "Nix packages, options, flakes and documentation";
        enabled = true;
        wrapper = "mcp-nixos";
      }
      {
        name = "filesystem";
        description = "Permission-scoped file access";
        enabled = true;
        wrapper = "mcp-filesystem";
      }
      {
        name = "dap";
        description = "Native debugger sessions";
        enabled = config.dotfiles.capabilities.development;
        wrapper = "mcp-dap";
      }
      {
        name = "rea";
        description = "Reverse engineering binaries, artifacts and runtimes";
        enabled = config.dotfiles.capabilities.development;
        wrapper = "mcp-rea";
        toolTimeout = 360;
      }
      {
        name = "slack";
        description = "Slack messages and people";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-slack";
      }
      {
        name = "telegram";
        description = "Telegram messages";
        enabled = config.dotfiles.capabilities.telegram;
        wrapper = "mcp-telegram";
      }
      {
        name = "gmail";
        description = "Personal email";
        enabled = config.dotfiles.capabilities.email;
        wrapper = "mcp-himalaya";
      }
      {
        name = "outlook";
        description = "Work email";
        enabled = config.dotfiles.capabilities.work && config.dotfiles.capabilities.playwright;
        wrapper = "mcp-outlook";
        toolTimeout = 360;
      }
      {
        name = "newrelic";
        description = "Read-only observability and alerts";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-newrelic";
      }
      {
        name = "aws-access-portal";
        description = "AWS access portal status and authorized credentials";
        enabled = config.dotfiles.capabilities.work;
        wrapper = "mcp-aws-access-portal";
        toolTimeout = 180;
      }
    ]
    ++ (
      if config.dotfiles.privateEnabled then
        map (server: server // { private = true; }) config.privateConfig.mcpServers
      else
        [ ]
    )
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
