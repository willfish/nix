{
  lib,
  buildGoModule,
  source,
}:
buildGoModule rec {
  pname = "mcp-dap-server";
  # flake.lock owns the revision, so the version follows the locked commit date.
  version =
    let
      d = source.lastModifiedDate;
    in
    "0-unstable-${builtins.substring 0 4 d}-${builtins.substring 4 2 d}-${builtins.substring 6 2 d}";

  src = source;
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
