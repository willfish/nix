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
  cudaPackages,
  autoAddDriverRunpath,
  cudaSupport ? false,
}:

let
  effectiveStdenv = if cudaSupport then cudaPackages.backendStdenv else stdenv;
in
effectiveStdenv.mkDerivation {
  pname = "pi-voice-audio";
  version = "0-unstable-2026-09-07";

  src = fetchFromGitHub {
    owner = "0xShug0";
    repo = "audio.cpp";
    rev = "a10738ad6622c3bb76bf459d6aae483cd2d8e87b";
    hash = "sha256-+VrxrXEqnsMOYaFhf7TkwIs2Bmcrim2Mv4KR92F9EWg=";
  };

  # Avoid overlapping decoder arenas when speech shares VRAM with local Qwen.
  patches = lib.optionals cudaSupport [
    ../config/voice/patches/qwen3-tts-release-decoder-graph.patch
  ];

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ]
  ++ lib.optionals cudaSupport [
    cudaPackages.cuda_nvcc
    autoAddDriverRunpath
  ];
  buildInputs = [
    openssl
  ]
  ++ lib.optionals cudaSupport (
    with cudaPackages;
    [
      cuda_cccl
      cuda_cudart
      libcublas
      libcufft
    ]
  )
  ++ lib.optionals (!cudaSupport) [
    vulkan-headers
    vulkan-loader
    glslang
    shaderc
  ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DENGINE_ENABLE_NATIVE_CPU=OFF"
    (lib.cmakeBool "ENGINE_ENABLE_CUDA" cudaSupport)
    (lib.cmakeBool "ENGINE_ENABLE_VULKAN" (!cudaSupport))
    "-DAUDIOCPP_DEPLOYMENT_BUILD=ON"
    "-DAUDIOCPP_BUILD_NATIVE_MODEL_MANAGER=ON"
    "-DAUDIOCPP_USE_SYSTEM_OPENSSL=ON"
    "-DAUDIOCPP_MODEL_SET=custom"
    "-DAUDIOCPP_MODELS=dots_tts,pocket_tts,supertonic,qwen3_tts"
  ]
  ++ lib.optionals cudaSupport [
    # RTX 5090 only, using the existing CUDA 12.9 toolchain.
    "-DCMAKE_CUDA_ARCHITECTURES=120a-real"
    "-DGGML_CUDA_NCCL=OFF"
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
    description = "Local GPU speech synthesis runtime for Pi voice";
    homepage = "https://github.com/0xShug0/audio.cpp";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "audiocpp_server";
  };
}
