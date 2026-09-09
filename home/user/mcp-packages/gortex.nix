{
  lib,
  stdenvNoCC,
  fetchurl,
}:

# Fixed native release; upgrade only after both runtime fixture modes pass.
# Release commit: 173cad8708b9877c52aea41a77fccaf1bfa15523.
stdenvNoCC.mkDerivation rec {
  pname = "gortex";
  version = "0.64.1";

  src = fetchurl {
    url = "https://github.com/zzet/gortex/releases/download/v${version}/gortex_linux_amd64.tar.gz";
    hash = "sha256-4vqUIzxJ94q9Tt9tHy43Qkoq4u8T/DvClBcYgKnsPgA=";
  };

  sourceRoot = ".";
  dontStrip = true;
  installPhase = ''
    runHook preInstall
    install -Dm755 gortex "$out/bin/gortex"
    install -Dm644 LICENSE.md "$out/share/licenses/gortex/LICENSE.md"
    runHook postInstall
  '';

  meta = {
    description = "Native code knowledge graph and MCP server (isolated trial)";
    homepage = "https://github.com/zzet/gortex";
    license = lib.licenses.asl20;
    mainProgram = "gortex";
    platforms = [ "x86_64-linux" ];
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
  };
}
