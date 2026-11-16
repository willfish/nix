{ pkgs, source }:
let
  inherit (pkgs) lib;
  version = (builtins.fromTOML (builtins.readFile "${source}/cli/Cargo.toml")).package.version;
  nativeVersion = "${version}-background.1";
  cargoLock = pkgs.writeText "agent-browser-Cargo.lock" (
    lib.replaceStrings [ ''version = "${version}"'' ] [ ''version = "${nativeVersion}"'' ] (
      builtins.readFile "${source}/cli/Cargo.lock"
    )
  );
  dashboard = pkgs.stdenvNoCC.mkDerivation {
    pname = "agent-browser-dashboard";
    inherit version;
    src = source;
    nativeBuildInputs = [
      pkgs.nodejs
      pkgs.pnpm
      pkgs.pnpmConfigHook
    ];
    pnpmWorkspaces = [ "dashboard" ];
    pnpmDeps = pkgs.fetchPnpmDeps {
      pname = "agent-browser-dashboard";
      inherit version;
      src = source;
      inherit (pkgs) pnpm;
      pnpmWorkspaces = [ "dashboard" ];
      fetcherVersion = 5;
      hash = "sha256-d/nrfdcv48d0u4OvYBOgteCZz1zO4wHl1RxGchyCAW4=";
    };
    NEXT_TELEMETRY_DISABLED = "1";
    postPatch = ''
      cp '${pkgs.geist-font.webfont}/share/fonts/woff2/Geist[wght].woff2' packages/dashboard/src/app/geist.woff2
      substituteInPlace packages/dashboard/src/app/layout.tsx \
        --replace-fail 'import { Geist } from "next/font/google";' 'import localFont from "next/font/local";' \
        --replace-fail 'Geist({ subsets: ["latin"], variable: "--font-sans" })' 'localFont({ src: "./geist.woff2", variable: "--font-sans" })'
    '';
    buildPhase = ''
      runHook preBuild
      pnpm --filter dashboard build
      runHook postBuild
    '';
    installPhase = ''
      mkdir -p "$out"
      cp -r packages/dashboard/out/. "$out/"
    '';
  };
  binary = pkgs.rustPlatform.buildRustPackage {
    pname = "agent-browser-unwrapped";
    version = nativeVersion;
    src = source;
    patches = [ ./background-safety.patch ];
    cargoLock.lockFile = cargoLock;
    cargoRoot = "cli";
    buildAndTestSubdir = "cli";
    checkType = "debug";
    nativeCheckInputs = [
      pkgs.which
      pkgs.writableTmpDirAsHomeHook
    ];
    checkFlags = [
      "--quiet"
      "--test-threads=1"
      # This upstream replay test implicitly launches Chrome. Browser checks
      # must use dedicated tabs in the existing visible Brave instance.
      "--skip=native::actions::tests::a_rejected_write_with_stripped_credentials_still_reaches_the_replay"
    ];
    preCheck = ''
      # The upstream async dispatcher exceeds libtest's default stack in debug builds.
      export RUST_MIN_STACK=33554432
    '';
    postPatch = ''
      substituteInPlace cli/Cargo.toml --replace-fail 'version = "${version}"' 'version = "${nativeVersion}"'
      cp ${cargoLock} cli/Cargo.lock
      cp -r ${dashboard} packages/dashboard/out
      substituteInPlace cli/src/native/browser.rs cli/src/native/cdp/obscura.rs \
        --replace-fail '"/bin/sleep"' '"${pkgs.coreutils}/bin/sleep"'
      substituteInPlace cli/src/native/e2e_tests.rs \
        --replace-fail '"/usr/bin/false"' '"${pkgs.coreutils}/bin/false"' \
        --replace-fail '"/bin/false"' '"${pkgs.coreutils}/bin/false"'
    '';
    # Native transport tests bind loopback sockets.
    __darwinAllowLocalNetworking = true;
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
    export AGENT_BROWSER_BACKGROUND=1
  ''
  + lib.optionalString pkgs.stdenv.isLinux (
    lib.replaceStrings [ "@binary@" ] [ "${binary}/bin/agent-browser" ] (
      builtins.readFile ./limit-memory.sh
    )
  )
  + ''
    exec ${binary}/bin/agent-browser "$@"
  '';
  passthru = {
    unwrapped = binary;
    inherit dashboard;
  };
  meta = {
    description = "Native background-only browser automation with owned-tab cleanup and bounded caches";
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
