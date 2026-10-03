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
  version = "5.0.0-unstable-2026-10-03";

  src = fetchFromGitHub {
    owner = "nicobailon";
    repo = "pi-mcp-adapter";
    rev = "d6ffcca34851dafe5278992f0284f5ab56479684";
    hash = "sha256-HnnVWrbtyt4Y8uLyjL+h4IsazBXR4/5Z54qzDKhvK14=";
  };

  # Nested Pi packages omit registry integrity. Upstream already carries the
  # hono, nanoid, postcss, protobufjs and smol-toml advisory fixes, and
  # settings.namespaceProxyTools replaces the old gateway-only patch.
  patches = [ ./pi-mcp-adapter-lock.patch ];
  npmDepsHash = "sha256-UHxcmehzUHMGqi0+ZLuqbq5C+ZvtlLSOCI8r0eiyHp4=";
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
