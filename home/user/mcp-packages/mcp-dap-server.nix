{
  lib,
  buildGoModule,
  fetchFromGitHub,
}:
buildGoModule rec {
  pname = "mcp-dap-server";
  version = "0-unstable-2026-09-23";

  src = fetchFromGitHub {
    owner = "go-delve";
    repo = "mcp-dap-server";
    rev = "5e4d952bd171ad670e1744b4f182a32fe00fa710";
    hash = "sha256-NrHsVTFzWR5U5OEAjpy+LtFlaEn5VvvMipV1WE54X7s=";
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
