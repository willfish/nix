{
  lib,
  stdenvNoCC,
  source,
  bun,
  cacert,
  fetchurl,
  autoPatchelfHook,
  makeWrapper,
  stdenv,
}:
let
  inherit (builtins.fromJSON (builtins.readFile "${source}/package.json")) version;
  nodeModules = stdenvNoCC.mkDerivation {
    pname = "ovis-node-modules";
    inherit version;
    src = source;
    nativeBuildInputs = [ bun ];
    dontConfigure = true;
    dontFixup = true;
    buildPhase = ''
      export HOME="$TMPDIR/home"
      export SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
      mkdir -p "$HOME"
      bun install --frozen-lockfile --ignore-scripts --no-cache
    '';
    installPhase = ''
      cp -R node_modules "$out"
    '';
    outputHashMode = "recursive";
    outputHashAlgo = "sha256";
    outputHash = "sha256-I04k5osKQhXYkPZWCH3LtPiflCIgYJhq2pk7NpumUno=";
  };
  sidecar = fetchurl {
    url = "https://github.com/devswha/ovis/releases/download/v${version}/ovis-pty-sidecar-linux-x64";
    sha256 = "044eb0dca08d844a9564d9a5f23129d538b755fd6fd4747f7966494d46676bd2";
  };
in
stdenvNoCC.mkDerivation {
  pname = "ovis";
  inherit version;
  src = source;
  patches = [ ./ovis-packages/remote-core.patch ];
  patchFlags = [
    "-p1"
    "--fuzz=0"
  ];
  nativeBuildInputs = [
    bun
    autoPatchelfHook
    makeWrapper
  ];
  buildInputs = [ stdenv.cc.cc.lib ];
  postPatch = ''
    cp ${./ovis-packages/remote-core.test.ts} server/dotfiles-remote-core.test.ts
  '';
  configurePhase = ''
    runHook preConfigure
    cp -R ${nodeModules} node_modules
    chmod -R u+w node_modules
    export HOME="$TMPDIR/home"
    mkdir -p "$HOME"
    runHook postConfigure
  '';
  buildPhase = ''
    runHook preBuild
    # Invoke the CLI through Bun; its upstream /usr/bin/env shebang is absent in the sandbox.
    bun node_modules/vite/bin/vite.js build --logLevel warn
    runHook postBuild
  '';
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    HERDR_TEST_MODE=unit bun test server/dotfiles-remote-core.test.ts
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    root="$out/lib/ovis"
    mkdir -p "$root/bin" "$out/bin"
    cp -R server shared integrations scripts dist node_modules package.json LICENSE "$root/"
    # These optional build dependencies target musl, not the hosts' glibc runtime.
    rm -r "$root/node_modules/lightningcss-linux-x64-musl" \
      "$root/node_modules/@rolldown/binding-linux-x64-musl"
    install -m 755 ${sidecar} "$root/bin/ovis-pty-sidecar"
    makeWrapper ${bun}/bin/bun "$out/bin/ovis" \
      --add-flags "$root/server/index.ts" \
      --set OVIS_PTY_SIDECAR_BIN "$root/bin/ovis-pty-sidecar"
    runHook postInstall
  '';
  passthru = { inherit bun nodeModules; };
  meta = {
    description = "Private browser and phone interface to Herdr sessions";
    homepage = "https://github.com/devswha/ovis";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "ovis";
  };
}
