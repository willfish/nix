{ pkgs }:
pkgs.writeShellApplication {
  name = "mcp-update";
  runtimeInputs = [
    pkgs.coreutils
    pkgs.curl
    pkgs.diffutils
    pkgs.jq
    pkgs.nix
    pkgs.nodejs
    pkgs.prefetch-npm-deps
  ];
  text = builtins.readFile ./mcp-update.sh;
  meta = {
    description = "Lock current MCP releases and refresh their runtime dependencies";
    mainProgram = "mcp-update";
    platforms = pkgs.lib.platforms.unix;
  };
}
