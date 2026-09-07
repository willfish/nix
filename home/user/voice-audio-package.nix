{
  lib,
  stdenv,
  fetchFromGitHub,
  cmake,
  ninja,
  pkg-config,
  openssl,
  vulkan-headers,
  vulkan-loader,
  glslang,
  shaderc,
}:

stdenv.mkDerivation {
  pname = "codex-voice-audio";
  version = "0-unstable-2026-09-07";

  src = fetchFromGitHub {
    owner = "0xShug0";
    repo = "audio.cpp";
    rev = "a10738ad6622c3bb76bf459d6aae483cd2d8e87b";
    hash = "sha256-+VrxrXEqnsMOYaFhf7TkwIs2Bmcrim2Mv4KR92F9EWg=";
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];
  buildInputs = [
    openssl
    vulkan-headers
    vulkan-loader
    glslang
    shaderc
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DENGINE_ENABLE_NATIVE_CPU=OFF"
    "-DENGINE_ENABLE_VULKAN=ON"
    "-DAUDIOCPP_DEPLOYMENT_BUILD=ON"
    "-DAUDIOCPP_BUILD_NATIVE_MODEL_MANAGER=ON"
    "-DAUDIOCPP_USE_SYSTEM_OPENSSL=ON"
    "-DAUDIOCPP_MODEL_SET=custom"
    "-DAUDIOCPP_MODELS=dots_tts,pocket_tts,supertonic,qwen3_tts"
  ];

  # Runtime only: packaged GGUF models need no Python, Torch or converters.
  installPhase = ''
    runHook preInstall
    install -Dm755 bin/audiocpp_cli $out/bin/audiocpp_cli
    install -Dm755 bin/audiocpp_server $out/bin/audiocpp_server
    install -Dm755 bin/audiocpp_model_manager $out/bin/audiocpp_model_manager
    runHook postInstall
  '';

  meta = {
    description = "Local Vulkan speech synthesis runtime for Codex voice";
    homepage = "https://github.com/0xShug0/audio.cpp";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "audiocpp_server";
  };
}
