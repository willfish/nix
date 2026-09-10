{
  lib,
  stdenvNoCC,
  fetchFromGitHub,
  pi-coding-agent,
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
  nativeCheckInputs = [ pi-coding-agent ];
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    export HOME="$TMPDIR/home"
    mkdir -p "$HOME"
    cp ${../../../tests/pi-prompt-history-profile.ts} __tests__/profiles.test.ts
    cp ${../../../tests/pi-prompt-history-runner.ts} __tests__/harness.ts
    # Run the unchanged upstream callbacks using the actual bundled Pi APIs.
    # Its compiled executable does not execute the node:test runner itself.
    for test in __tests__/*.test.ts; do
      substituteInPlace "$test" --replace-fail 'from "node:test"' 'from "./harness"'
      printf 'import "./%s";\n' "$test" >> check.ts
    done
    printf 'export { run as default } from "./__tests__/harness";\n' >> check.ts
    for profile in standard qwen; do
      if [ "$profile" = standard ]; then
        unset PI_CODING_AGENT_DIR
      else
        export PI_CODING_AGENT_DIR="$HOME/.config/local-llm/pi"
      fi
      pi --offline --no-extensions -e "$PWD/check.ts" \
        --list-models __history_tests__ > "$TMPDIR/$profile.log" 2>&1
      cat "$TMPDIR/$profile.log"
      # Pi reports extension-load errors without a non-zero process exit.
      grep -q '^HISTORY_TESTS_PASSED=' "$TMPDIR/$profile.log"
      ! grep -q '^FAIL ' "$TMPDIR/$profile.log"
    done
    runHook postCheck
  '';

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
