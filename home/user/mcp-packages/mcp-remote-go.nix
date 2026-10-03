{
  lib,
  buildGoModule,
  fetchFromGitHub,
}:
buildGoModule rec {
  pname = "mcp-remote-go";
  version = "0.2.3-unstable-2026-09-17";

  src = fetchFromGitHub {
    owner = "naotama2002";
    repo = "mcp-remote-go";
    # Newest commit that still builds with NixOS 26.05's Go 1.26.
    # HEAD requires Go 1.27.
    rev = "983541e6584e8b055d97b1cb3806bdfebf51b518";
    hash = "sha256-PPTnccCq7a4tbkq54XrvqeogXO0/8wq61uS6GgmfKTA=";
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
    #
    # TestConcurrentAuthOperations races a 5s collect timeout against 5s
    # exclusive lock-file waits. Under sandbox scheduling the wait-group can
    # return after the context expires, so the select fails even when every
    # save/load finished.
    go test ./auth -skip TestConcurrentAuthOperations
    # Renewal tests seed ~/.mcp-remote-go-auth. A preset config dir hides that
    # home. The 401 renewal test dials a public host the sandbox never reaches.
    env -u MCP_REMOTE_CONFIG_DIR HOME="$TMPDIR/mcp-remote-home" \
      go test ./cmd/... ./internal/... ./proxy \
      -skip TestRenewsOn401WithoutAskingTheUser
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
