{
  lib,
  buildGoModule,
  source,
}:
buildGoModule rec {
  pname = "slack-mcp-server";
  # flake.lock owns the revision, so the version follows the locked commit date.
  version =
    let
      d = source.lastModifiedDate;
    in
    "0-unstable-${builtins.substring 0 4 d}-${builtins.substring 4 2 d}-${builtins.substring 6 2 d}";

  src = source;
  vendorHash = "sha256-+uQRODO9oL8mGKBmdghTxE6R9Fz+3GJFVTi17306gT8=";
  subPackages = [ "cmd/slack-mcp-server" ];
  env.CGO_ENABLED = 0;
  ldflags = [
    "-s"
    "-w"
    "-X github.com/korotovsky/slack-mcp-server/pkg/version.Version=v${version}"
  ];

  # Upstream integration tests require a Slack workspace and ngrok.
  # The built executable is covered by our offline MCP runtime regression.
  doCheck = false;

  meta = {
    description = "Native Slack MCP server with browser-session authentication";
    homepage = "https://github.com/korotovsky/slack-mcp-server";
    license = lib.licenses.mit;
    mainProgram = "slack-mcp-server";
    platforms = lib.platforms.unix;
  };
}
