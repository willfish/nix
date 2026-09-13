{
  lib,
  buildNpmPackage,
  fetchFromGitHub,
  stdenv,
  autoPatchelfHook,
  zlib,
}:
buildNpmPackage {
  pname = "pi-mcp-adapter";
  version = "2.33.0-unstable-2026-09-13";

  # Includes post-release OAuth and live metadata fixes.
  src = fetchFromGitHub {
    owner = "nicobailon";
    repo = "pi-mcp-adapter";
    rev = "464337bc9be7e0806756812d206d9ca0a7be1d5e";
    hash = "sha256-Jlzo/5A5qWl/sNc8z9jXLYdw8u3n1u/CuFpOKmatfoI=";
  };

  # Restore missing registry integrity before applying compatible security fixes.
  patches = [
    ./pi-mcp-adapter-lock.patch
    ./pi-mcp-adapter-security.patch
    ./pi-mcp-adapter-gateway-only.patch
  ];
  npmDepsHash = "sha256-0RnuypCZ3B16JYq840MlhHRE0bPeVOrvsJv7UAVVly4=";
  # npm needs to update cache entries shared by nested Pi dev dependencies.
  makeCacheWritable = true;
  npmFlags = [ "--ignore-scripts" ];
  npmBuildScript = "build:public";
  nativeBuildInputs = lib.optionals stdenv.isLinux [ autoPatchelfHook ];
  buildInputs = lib.optionals stdenv.isLinux [
    stdenv.cc.cc.lib
    zlib
  ];
  postInstall = lib.optionalString stdenv.isLinux ''
    # npm installs both libc variants; NixOS uses the GNU build.
    rm -rf "$out/lib/node_modules/pi-mcp-adapter/node_modules/@napi-rs/"*-musl
  '';

  # Pi loads the TypeScript entry and supplies its own host API peer modules.
  # Our offline runtime test exercises the packaged extension in the actual Pi.
  meta = {
    description = "MCP discovery and calls on demand for Pi";
    homepage = "https://github.com/nicobailon/pi-mcp-adapter";
    license = lib.licenses.mit;
    platforms = lib.platforms.unix;
  };
}
