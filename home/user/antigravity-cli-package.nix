{
  lib,
  stdenvNoCC,
  fetchurl,
  autoPatchelfHook,
  makeWrapper,
  glibc,
}:
stdenvNoCC.mkDerivation {
  pname = "antigravity-cli";
  version = "1.5.7";
  # Google's installer resolves this release through its platform manifest.
  src = fetchurl {
    url = "https://storage.googleapis.com/antigravity-public/antigravity-cli/1.5.7-6190022064472064/linux-x64/cli_linux_x64.tar.gz";
    hash = "sha512-YLyigEFO0Rcj9Ys7ZxlirjlMwtDCXj9Y4y4caEfJeMvHfsb0Dn85kgAcOZ2Arri2TJ7Ikqvd1ZGxJBPF7m2fzg==";
  };
  sourceRoot = ".";
  nativeBuildInputs = [
    autoPatchelfHook
    makeWrapper
  ];
  buildInputs = [ glibc ];
  dontBuild = true;
  installPhase = ''
    runHook preInstall
    install -Dm755 antigravity "$out/bin/agy"
    runHook postInstall
  '';
  postFixup = ''
    wrapProgram "$out/bin/agy" --set AGY_CLI_DISABLE_AUTO_UPDATE true
  '';
  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck
    "$out/bin/agy" --version
    "$out/bin/agy" --help >/dev/null
    runHook postInstallCheck
  '';
  meta = {
    description = "Google Antigravity terminal coding agent";
    homepage = "https://antigravity.google/product/antigravity-cli";
    license = lib.licenses.unfree;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = [ "x86_64-linux" ];
    mainProgram = "agy";
  };
}
