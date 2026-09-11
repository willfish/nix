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
  version = "2.32.1-unstable-2026-09-08";

  # Includes upstream's Pi 0.85 compatibility update after the 2.32.1 tag.
  src = fetchFromGitHub {
    owner = "nicobailon";
    repo = "pi-mcp-adapter";
    rev = "8243eba3421e301c88c047444f34ab7d5d57163e";
    hash = "sha256-Z+Nc7aQJFnZKYAe6yQN0CFwYuekNahAcFRg+dDBpRVU=";
  };

  # Upstream omitted six existing dev dependencies' registry integrity hashes.
  # Restore those hashes without changing any dependency versions or URLs.
  patches = [
    ./pi-mcp-adapter-lock.patch
    ./pi-mcp-adapter-gateway-only.patch
  ];
  npmDepsHash = "sha256-hYq5a4Y/IzcG70QhgdU+LJLDZfCe9kFAFj76VsmgFkw=";
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
