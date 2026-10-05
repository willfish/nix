{
  lib,
  stdenvNoCC,
  fetchFromGitHub,
}:
stdenvNoCC.mkDerivation {
  pname = "pi-prompt-history";
  version = "0.1.0-unstable-eedbef7";

  src = fetchFromGitHub {
    owner = "vedang";
    repo = "pi-prompt-history";
    rev = "eedbef7afdf16a317785be469f600d71fadc9ef0";
    hash = "sha256-7tw7LMB/pzLrDzRTWd+jHaqLUMxXu1MSysIkra8s/a4=";
  };

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
