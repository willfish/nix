{
  lib,
  stdenv,
  buildNpmPackage,
  fetchurl,
  autoPatchelfHook,
}:

let
  version = "6.4.0";
in
buildNpmPackage {
  pname = "rea-agents";
  inherit version;

  # The release includes compiled JavaScript and provider bridges. Keep only
  # runtime dependencies in the lock; never run upstream setup or build scripts.
  src = fetchurl {
    url = "https://registry.npmjs.org/rea-agents/-/rea-agents-${version}.tgz";
    hash = "sha256-WesWBLjbdfUi1Kl1w2zmtKKtRl5DutFl+aj3KDkdubg=";
  };
  sourceRoot = "package";

  npmDepsFetcherVersion = 2;
  npmDepsHash = "sha256-rJD1sZkbopd1O+eQQ7o/jlk/0u9zEXvXvBBXFY91mkw=";
  npmFlags = [ "--ignore-scripts" ];
  dontNpmBuild = true;

  postPatch = ''
    cp ${./rea-agents-package.json} package.json
    cp ${./rea-agents-package-lock.json} package-lock.json
  '';

  # REA's optional process capture uses the prebuilt node-pty addon.
  nativeBuildInputs = lib.optionals stdenv.hostPlatform.isLinux [ autoPatchelfHook ];
  buildInputs = lib.optionals stdenv.hostPlatform.isLinux [ stdenv.cc.cc.lib ];

  meta = {
    description = "Local reverse-engineering CLI and MCP server";
    homepage = "https://github.com/morluto/rea";
    license = lib.licenses.mit;
    mainProgram = "rea";
    platforms = lib.platforms.unix;
  };
}
