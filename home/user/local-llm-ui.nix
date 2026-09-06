{ pkgs }:
# Build only the UI from the same flake-pinned source/dependencies as the server.
# Upstream hook changes fail at patch time rather than silently disabling compaction.
pkgs.stdenvNoCC.mkDerivation {
  pname = "local-llm-ui";
  inherit (pkgs.llama-cpp) version src npmDeps;
  npmRoot = "tools/ui";
  nativeBuildInputs = [
    pkgs.nodejs_latest
    pkgs.npmHooks.npmConfigHook
  ];
  npm_config_ignore_scripts = "true";
  npmFlags = [ "--ignore-scripts" ];
  patches = [ ../config/local-llm/ui-compaction.patch ];
  postPatch = ''
    mkdir -p tools/ui/src/lib/local-compaction
    cp ${../config/local-llm/compaction.ts} tools/ui/src/lib/local-compaction/compaction.ts
    cp ${../config/local-llm/compaction-transport.ts} tools/ui/src/lib/local-compaction/compaction-transport.ts
    cp ${../config/local-llm/compaction-browser.ts} tools/ui/src/lib/local-compaction/compaction-browser.ts
  '';
  buildPhase = ''
    runHook preBuild
    pushd tools/ui
    # Reviewed upstream scripts; no dependency lifecycle scripts are enabled.
    npm run check
    npm run test:unit -- --run
    LLAMA_BUILD_NUMBER=${pkgs.llama-cpp.version} npm run build
    popd
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    cp -r build/tools/ui/dist/. "$out/"
    test -s "$out/index.html"
    test -s "$out/bundle.js"
    runHook postInstall
  '';
}
