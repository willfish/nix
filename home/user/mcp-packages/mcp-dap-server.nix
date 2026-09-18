{
  lib,
  buildGoModule,
  fetchFromGitHub,
}:
buildGoModule rec {
  pname = "mcp-dap-server";
  version = "0-unstable-2026-07-23";

  src = fetchFromGitHub {
    owner = "go-delve";
    repo = "mcp-dap-server";
    rev = "ca7f841a8ab2311ec8c533153c0802e9a68785d3";
    hash = "sha256-bx5H305MJyiqGKiqV5NoL9PoQW10fPbm1M9Mh+2yiX4=";
  };
  vendorHash = "sha256-/1dBkkz/YuCYzlneLVPYtjjCkzSY2lpYzcnzP/6CXGo=";
  env.CGO_ENABLED = 0;
  ldflags = [
    "-s"
    "-w"
  ];

  # Upstream tests spawn Delve and language debug adapters.
  doCheck = false;

  meta = {
    description = "MCP server that drives DAP debuggers";
    homepage = "https://github.com/go-delve/mcp-dap-server";
    license = lib.licenses.mit;
    mainProgram = "mcp-dap-server";
    platforms = lib.platforms.unix;
  };
}
