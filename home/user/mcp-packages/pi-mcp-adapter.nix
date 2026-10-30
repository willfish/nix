{
  lib,
  buildNpmPackage,
  stdenv,
  autoPatchelfHook,
  zlib,
  source,
}:
buildNpmPackage {
  pname = "pi-mcp-adapter";
  version =
    let
      pkg = lib.importJSON "${source}/package.json";
    in
    "${pkg.version}-unstable-${source.shortRev}";

  src = source;

  # Nested Pi packages omit registry integrity. Upstream already carries the
  # hono, nanoid, postcss, protobufjs and smol-toml advisory fixes, and
  # settings.namespaceProxyTools replaces the old gateway-only patch.
  patches = [ ./pi-mcp-adapter-lock.patch ];
  npmDepsHash = "sha256-Sfufw0ee8c/z7aQIXjNn8LQNxlHSWAXfqSBH5BqMftY=";
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
