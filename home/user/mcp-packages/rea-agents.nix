{
  lib,
  stdenv,
  buildNpmPackage,
  fetchurl,
  autoPatchelfHook,
}:

let
  version = "6.7.0";
in
buildNpmPackage {
  pname = "rea-agents";
  inherit version;

  # The release includes compiled JavaScript and provider bridges. Keep only
  # runtime dependencies in the lock; never run upstream setup or build scripts.
  src = fetchurl {
    url = "https://registry.npmjs.org/rea-agents/-/rea-agents-${version}.tgz";
    hash = "sha256-IJWKZtuhoBHlCkhr65Rjo0MSjtNoyYT5QGU2LBP1H5A=";
  };
  sourceRoot = "package";

  npmDepsFetcherVersion = 2;
  npmDepsHash = "sha256-ZeKg1n28dhI5i/jbGNEjgKtpRnl89GzfBueziyAWdbw=";
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
