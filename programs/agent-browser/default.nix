{ pkgs, source }:
let
  inherit (pkgs) lib;
  binary = pkgs.stdenvNoCC.mkDerivation {
    pname = "agent-browser-unwrapped";
    version = "rolling";
    src = source.outPath;
    dontUnpack = true;
    installPhase = ''
      runHook preInstall
      install -Dm755 "$src" "$out/bin/agent-browser"
      runHook postInstall
    '';
  };
in
pkgs.writeShellApplication {
  name = "agent-browser";
  runtimeInputs = lib.optionals pkgs.stdenv.isLinux [
    pkgs.systemd
    pkgs.gawk
    pkgs.gnugrep
  ];
  text = ''
    export AGENT_BROWSER_IDLE_TIMEOUT_MS=900000
  ''
  + lib.optionalString pkgs.stdenv.isLinux (
    lib.replaceStrings [ "@binary@" ] [ "${binary}/bin/agent-browser" ] (
      builtins.readFile ./limit-memory.sh
    )
  )
  + ''
    exec ${binary}/bin/agent-browser "$@"
  '';
  passthru.unwrapped = binary;
  meta = {
    description = "Browser automation with idle cleanup and shared Linux memory limits";
    homepage = "https://github.com/vercel-labs/agent-browser";
    license = lib.licenses.asl20;
    mainProgram = "agent-browser";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
      "x86_64-darwin"
      "aarch64-darwin"
    ];
  };
}
