{
  lib,
  buildNpmPackage,
  fetchurl,
  source,
}:

let
  release = lib.importJSON source.outPath;
  runtime = lib.importJSON ./brave-search-mcp-package.json;
in
assert lib.assertMsg (
  release.version == runtime.version && release.dependencies == runtime.dependencies
) "Brave Search runtime lock needs refreshing: run nix run .#mcp-update -- .";
assert lib.assertMsg
  (lib.hasPrefix "https://registry.npmjs.org/@brave/brave-search-mcp-server/-/" release.dist.tarball)
  "Brave Search release must come from its official npm registry path";
buildNpmPackage {
  pname = "brave-search-mcp-server";
  inherit (release) version;

  # Official npm pack includes dist/. The GitHub tag lockfile omits registry URLs, so
  # Nix fetches dependencies from this production lock instead of rebuilding TypeScript.
  src = fetchurl {
    url = release.dist.tarball;
    hash = release.dist.integrity;
  };
  sourceRoot = "package";

  npmDepsFetcherVersion = 2;
  npmDepsHash = (lib.importJSON ./brave-search-mcp-deps.json).hash;
  npmFlags = [ "--ignore-scripts" ];

  postPatch = ''
    cp ${./brave-search-mcp-package.json} package.json
    cp ${./brave-search-mcp-package-lock.json} package-lock.json
  '';

  dontNpmBuild = true;

  meta = {
    description = "Official Brave Search MCP server";
    homepage = "https://github.com/brave/brave-search-mcp-server";
    license = lib.licenses.mit;
    mainProgram = "brave-search-mcp-server";
    platforms = lib.platforms.unix;
  };
}
