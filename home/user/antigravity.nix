{
  config,
  lib,
  pkgs,
  ...
}:
let
  llmMcps = import ./llm-mcps.nix { inherit config pkgs; };
  # agy reads global MCP servers from this file. It lists every tool name in
  # the system prompt and loads schemas lazily through call_mcp_tool.
  mcpServers = builtins.listToAttrs (
    map (server: {
      inherit (server) name;
      value =
        if llmMcps.isHttp server then
          { inherit (server) url; }
        else
          { command = llmMcps.commandFor server; };
    }) llmMcps.servers
  );
in
lib.mkIf (pkgs.stdenv.isLinux && config.dotfiles.capabilities.desktop) {
  home.file.".gemini/config/mcp_config.json".text = builtins.toJSON { inherit mcpServers; };
}
