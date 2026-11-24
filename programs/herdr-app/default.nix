{
  lib,
  stdenv,
  fetchurl,
  autoPatchelfHook,
  copyDesktopItems,
  makeDesktopItem,
  libglvnd,
  libxcb,
  libxkbcommon,
  vulkan-loader,
  wayland,
}:
stdenv.mkDerivation (finalAttrs: {
  pname = "herdr-app";
  version = "0.1.17";

  src = fetchurl {
    url = "https://releases.herdr.dev/app/${finalAttrs.version}/herdr-app-linux-x86_64.tar.gz";
    sha256 = "2188f4d7d5cf041450dbdbf2e14649244b1a6274a1080bdd97fa7bc450e65ca1";
  };

  nativeBuildInputs = [
    autoPatchelfHook
    copyDesktopItems
  ];
  buildInputs = [
    libxcb
    libxkbcommon
    stdenv.cc.cc.lib
  ];
  # Loaded by name when the app opens a window, not listed in the ELF dependencies.
  runtimeDependencies = [
    libglvnd
    vulkan-loader
    wayland
  ];

  dontBuild = true;
  desktopItems = [
    (makeDesktopItem {
      name = "herdr-app";
      desktopName = "Herdr";
      comment = "Terminals and coding agents that keep running";
      exec = "herdr-app";
      icon = "herdr-app";
      startupWMClass = "herdr-app";
      categories = [ "Development" ];
    })
  ];

  installPhase = ''
    runHook preInstall
    install -Dm755 herdr-app "$out/bin/herdr-app"
    # Keep the icon away from the binary so the upstream in-place updater
    # does not recognize this as a writable release install.
    install -Dm644 herdr-app.svg "$out/share/icons/hicolor/scalable/apps/herdr-app.svg"
    install -Dm644 -t "$out/share/doc/herdr-app" LICENSE THIRD-PARTY-NOTICES
    runHook postInstall
  '';

  meta = {
    description = "Desktop app for terminals and coding agents";
    homepage = "https://herdr.dev/";
    license = lib.licenses.asl20;
    mainProgram = "herdr-app";
    platforms = [ "x86_64-linux" ];
  };
})
