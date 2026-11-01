{
  lib,
  stdenv,
  source,
  runCommand,
  python3,
  cacert,
  fetchurl,
  autoPatchelfHook,
  cmake,
  pkg-config,
  makeWrapper,
  sbcl,
  openssl,
  libfixposix,
  enchant,
  sqlite,
  wayland,
  wayland-scanner,
  libx11,
  libxcb,
  libxcomposite,
  libxdamage,
  libxext,
  libxfixes,
  libxrandr,
  libxkbcommon,
  glib,
  gtk3,
  nss,
  nspr,
  atk,
  at-spi2-atk,
  cups,
  dbus,
  expat,
  alsa-lib,
  pango,
  cairo,
  libdrm,
  libgbm,
  mesa,
  systemd,
  libGL,
  xdg-utils,
  wl-clipboard,
  xclip,
}:
let
  version = "4.0.0-unstable-${source.lastModifiedDate or "20261101"}";
  dependencies =
    runCommand "nyxt-lisp-sources"
      {
        nativeBuildInputs = [ python3 ];
        outputHashMode = "recursive";
        outputHashAlgo = "sha256";
        outputHash = "sha256-3AXntYlEcVFHV3LpEn2KUQmebAF8Yxh0e3SRmLjStjY=";
      }
      ''
        export NIX_SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
        export SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
        python3 ${./fetch-dependencies.py} ${source}/ocicl.csv "$out"
      '';
  cefArchive = fetchurl {
    url = "https://cef-builds.spotifycdn.com/cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_linux64_minimal.tar.bz2";
    hash = "sha256-3Rtcw6xu/mWex+jpsGd7pOQyFqL+/K/Vq8k0CDfjLZY=";
  };
  runtimeLibraries = [
    openssl
    libfixposix
    enchant
    sqlite
    wayland
    libx11
    libxcb
    libxcomposite
    libxdamage
    libxext
    libxfixes
    libxrandr
    libxkbcommon
    glib
    gtk3
    nss
    nspr
    atk
    at-spi2-atk
    cups
    dbus
    expat
    alsa-lib
    pango
    cairo
    libdrm
    libgbm
    mesa
    systemd
    libGL
    stdenv.cc.cc.lib
  ];
  cefBridge = stdenv.mkDerivation {
    pname = "nyxt-cef";
    inherit version;
    src = dependencies;
    nativeBuildInputs = [
      autoPatchelfHook
      cmake
    ];
    buildInputs = runtimeLibraries;
    LD_LIBRARY_PATH = lib.makeLibraryPath runtimeLibraries;
    dontUseCmakeConfigure = true;
    dontStrip = true;
    unpackPhase = ''
      runHook preUnpack
      mkdir source cef-runtime
      cp -r ${dependencies}/cl-cef-*/* source/
      chmod -R u+w source
      tar -xjf ${cefArchive} -C cef-runtime --strip-components=1
      export CEF_ROOT="$PWD/cef-runtime"
      cd source
      runHook postUnpack
    '';
    buildPhase = ''
      runHook preBuild
      cmake -S . -B build -DCEF_ROOT="$CEF_ROOT" -DCMAKE_BUILD_TYPE=Release
      cmake --build build --parallel "$NIX_BUILD_CORES"
      runHook postBuild
    '';
    installPhase = ''
      runHook preInstall
      mkdir -p "$out/lib/nyxt/cef"
      cp -r build/{libcl-cef.so,cl-cef-helper,libcef.so,libEGL.so,libGLESv2.so,libvk_swiftshader.so,libvulkan.so.1,v8_context_snapshot.bin,vk_swiftshader_icd.json,chrome_100_percent.pak,chrome_200_percent.pak,resources.pak,icudtl.dat,locales} "$out/lib/nyxt/cef/"
      for binary in libcl-cef.so cl-cef-helper; do
        patchelf --set-rpath "$out/lib/nyxt/cef:${lib.makeLibraryPath runtimeLibraries}" \
          "$out/lib/nyxt/cef/$binary"
      done
      runHook postInstall
    '';
    preFixup = ''
      addAutoPatchelfSearchPath "$out/lib/nyxt/cef"
    '';
  };
in
stdenv.mkDerivation (finalAttrs: {
  pname = "nyxt";
  inherit version;
  src = source;
  patches = [ ./nonblocking-ui-buttons.patch ];

  nativeBuildInputs = [
    pkg-config
    makeWrapper
    sbcl
    wayland-scanner
  ];
  buildInputs = runtimeLibraries;
  dontUseCmakeConfigure = true;
  dontStrip = true; # Stripping corrupts SBCL's embedded image.
  dontPatchELF = true; # Patchelf also destroys the appended SBCL image.

  NYXT_VERSION = version;
  NYXT_RENDERER = "cef";
  NASDF_USE_LOGICAL_PATHS = "true";
  LD_LIBRARY_PATH = lib.makeLibraryPath runtimeLibraries;

  postUnpack = ''
    cp -r ${dependencies} "$sourceRoot/ocicl"
    chmod -R u+w "$sourceRoot/ocicl"
  '';

  postPatch = ''
    # Keep runtime source lookups valid after the build directory disappears.
    export CL_CEF_SOURCE="$PWD/ocicl/$(dirname "$(awk -F', ' '$1 == "cl-cef" {print $3; exit}' ocicl.csv)")"
    substituteInPlace source/renderer/cef.lisp \
      --replace-fail "(make-instance 'cef:interface)" \
        "(make-instance 'cef:interface :no-sandbox-p nil)"
    substituteInPlace "$CL_CEF_SOURCE/source/lisp/core.lisp" \
      --replace-fail '(asdf:system-relative-pathname :cl-cef "build/cl-cef-helper")' \
        '#p"${cefBridge}/lib/nyxt/cef/cl-cef-helper"' \
      --replace-fail '(asdf:system-relative-pathname :cl-cef "build/")' \
        '#p"${cefBridge}/lib/nyxt/cef/"'
  '';

  buildPhase = ''
    runHook preBuild
    export HOME="$TMPDIR/home"
    mkdir -p "$HOME"
    sbcl --dynamic-space-size 4096 --no-userinit --non-interactive \
      --load ${./build.lisp}
    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    if ! ./nyxt --no-config --no-auto-config --quit \
      --load ${./check-ui-callbacks.lisp} > ui-callback-check.log 2>&1; then
      cat ui-callback-check.log
      exit 1
    fi
    cat ui-callback-check.log
    grep -q '^NYXT_UI_CALLBACKS_OK:' ui-callback-check.log
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 nyxt "$out/bin/nyxt"
    mkdir -p "$out/lib/nyxt" "$out/share/nyxt"
    find ocicl -name libcl-clipboard.so -exec install -m755 {} "$out/lib/nyxt/" \;
    cp -r source libraries nyxt.asd "$out/share/nyxt/"
    install -Dm644 assets/nyxt.desktop "$out/share/applications/nyxt.desktop"
    install -Dm644 assets/nyxt.metainfo.xml "$out/share/metainfo/nyxt.metainfo.xml"
    install -Dm644 assets/glyphs/nyxt.svg "$out/share/icons/hicolor/scalable/apps/nyxt.svg"
    for size in 16 32 128 256 512; do
      install -Dm644 "assets/icons/nyxt_''${size}x''${size}.png" \
        "$out/share/icons/hicolor/''${size}x''${size}/apps/nyxt.png"
    done
    runHook postInstall
  '';

  preFixup = ''
    wrapProgram "$out/bin/nyxt" \
      --set CL_CEF_LIBRARY "${cefBridge}/lib/nyxt/cef/libcl-cef.so" \
      --prefix LD_LIBRARY_PATH : "$out/lib/nyxt:${cefBridge}/lib/nyxt/cef:${lib.makeLibraryPath runtimeLibraries}" \
      --prefix PATH : ${
        lib.makeBinPath [
          xdg-utils
          wl-clipboard
          xclip
        ]
      }
  '';

  passthru = {
    inherit dependencies;
    tests.version = runCommand "nyxt-version-check" { } ''
      set -o pipefail
      export HOME="$TMPDIR"
      ${finalAttrs.finalPackage}/bin/nyxt --version \
        | grep -Fx ${lib.escapeShellArg "Nyxt version ${version}"}
      touch "$out"
    '';
  };
  meta = {
    description = "Extensible Lisp web browser built from source with the CEF renderer";
    homepage = "https://github.com/atlas-engineer/nyxt";
    license = lib.licenses.bsd3;
    mainProgram = "nyxt";
    platforms = [ "x86_64-linux" ];
  };
})
