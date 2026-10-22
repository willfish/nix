{
  lib,
  stdenvNoCC,
  source,
}:
stdenvNoCC.mkDerivation {
  pname = "pi-prompt-history";
  version =
    let
      pkg = lib.importJSON "${source}/package.json";
    in
    "${pkg.version}-unstable-${source.shortRev}";

  src = source;

  # Upstream hardcodes ~/.pi/agent, including in its bundled config.json.
  # Respect Pi's active profile for defaults and per-profile overrides instead.
  patches = [
    ./profile-paths.patch
    # Gate project overrides on Pi trust and block the incompatible cross-session fork.
    ./pi-safety.patch
  ];
  dontBuild = true;
  doCheck = false;

  installPhase = ''
    runHook preInstall
    mkdir -p "$out"
    cp -r src config.json package.json LICENSE.txt "$out/"
    # Pi auto-discovers extensions/<directory>/index.ts, not src/index.ts.
    printf 'export { default } from "./src/index";\n' > "$out/index.ts"
    runHook postInstall
  '';

  meta = {
    description = "Profile-aware fuzzy prompt history overlay for Pi";
    homepage = "https://github.com/vedang/pi-prompt-history";
    license = lib.licenses.wtfpl;
    platforms = lib.platforms.unix;
  };
}
