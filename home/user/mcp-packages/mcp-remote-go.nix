{
  lib,
  buildGoModule,
  fetchFromGitHub,
}:
buildGoModule rec {
  pname = "mcp-remote-go";
  version = "0.2.3-unstable-2026-08-07";

  src = fetchFromGitHub {
    owner = "naotama2002";
    repo = "mcp-remote-go";
    # Includes protocol stdout and idle-stdin shutdown fixes after v0.2.3.
    rev = "e6e1e26c7ef053b134aa95ddde7f79c6dc65bdcd";
    hash = "sha256-cyDm71iim72nRmLtQmQc319ERVxhL4qAEDatbCJVjDc=";
  };
  vendorHash = "sha256-CjlrMGRHnF4B5gEsExFcr7FkL8LDukWQHhzLEm+zHBw=";
  # Explicit bearer headers must take precedence over cached interactive OAuth.
  patches = [ ./mcp-remote-go-auth.patch ];
  subPackages = [ "cmd/mcp-remote-go" ];
  env.CGO_ENABLED = 0;
  ldflags = [
    "-s"
    "-w"
    "-X main.version=${version}"
    "-X main.gitCommit=${src.rev}"
  ];

  checkPhase = ''
    runHook preCheck
    # Auth tests isolate their own homes. The other suites start subprocesses
    # which need a writable config directory inside the Nix sandbox.
    go test ./auth
    MCP_REMOTE_CONFIG_DIR="$TMPDIR/mcp-remote-test-auth" \
      go test ./cmd/... ./internal/... ./proxy
    runHook postCheck
  '';

  meta = {
    description = "Native stdio to remote HTTP MCP proxy";
    homepage = "https://github.com/naotama2002/mcp-remote-go";
    license = lib.licenses.mit;
    mainProgram = "mcp-remote-go";
    platforms = lib.platforms.unix;
  };
}
