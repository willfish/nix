{
  lib,
  pkgs,
  home,
}:
let
  tools = [
    pkgs.bash
    pkgs.coreutils
    pkgs.fd
    pkgs.flock
    pkgs.ripgrep
  ];
  source = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ./pi-agent-bus-composition.py
      ./test_pi_agent_bus_wiring.py
      ./fixtures/pi-agent-bus-composition
      ../home/user/pi.nix
      ../home/user/local-llm.nix
      ../home/user/prompt-capture.sh
      ../system/terminus/configuration.nix
      ../docs/nixos-host-operations.md
    ];
  };
in
pkgs.runCommand "pi-agent-bus-consumer-composition"
  {
    nativeBuildInputs = [ pkgs.python3 ] ++ tools;
    preferLocalBuild = true;
    allowSubstitutes = false;
    __darwinAllowLocalNetworking = true;
  }
  ''
    export HOME="$TMPDIR/home"
    mkdir -p "$HOME"
    cp -R ${source} source
    chmod -R u+w source
    cd source
    python3 tests/pi-agent-bus-composition.py \
      --pi-package ${pkgs.pi-coding-agent} \
      --pi-version ${lib.escapeShellArg pkgs.pi-coding-agent.version} \
      --extension-package ${home.programs.pi-agent-bus.package} \
      --prompt-history ${home.home.file.".pi/agent/extensions/prompt-history".source} \
      --mitmdump ${pkgs.mitmproxy}/bin/mitmdump \
      --ca-bundle ${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt \
      --tool-path ${lib.makeBinPath tools}
    touch "$out"
  ''
