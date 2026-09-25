{
  lib,
  buildNpmPackage,
  fetchurl,
}:

let
  version = "2.1.4";
in
buildNpmPackage {
  pname = "brave-search-mcp-server";
  inherit version;

  # Official npm pack includes dist/. The GitHub tag lockfile omits registry URLs, so
  # Nix fetches dependencies from this production lock instead of rebuilding TypeScript.
  src = fetchurl {
    url = "https://registry.npmjs.org/@brave/brave-search-mcp-server/-/brave-search-mcp-server-${version}.tgz";
    hash = "sha256-V3DwkfwV16Sfn0QATjl5P9gSakaEE4vmYq5o9KEN/yc=";
  };
  sourceRoot = "package";

  npmDepsFetcherVersion = 2;
  npmDepsHash = "sha256-FX53NPDgsJxXJ/rAaNTKaOHN/a3wW4k+9dnj6jvyVRk=";

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
