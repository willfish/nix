{ pkgs }:
let
  cuda = pkgs.cudaPackages_13_0;
  llamaSource = pkgs.fetchFromGitHub {
    owner = "ggml-org";
    repo = "llama.cpp";
    rev = "3cf03257f219afbe7334045ff7c6a06ac68c627d";
    sha256 = "10hr8pkszphsylrg3j055pb0s4v2qpaip6kp0x12005qmxfsh4a9";
  };
  python = pkgs.python3.withPackages (
    ps: with ps; [
      numpy
      jinja2
      regex
      pyyaml
      tqdm
      requests
      pillow
      psutil
    ]
  );
in
pkgs.stdenv.mkDerivation {
  pname = "strata";
  passthru = { inherit python llamaSource; };
  version = "0.1.40";
  src = pkgs.fetchFromGitHub {
    owner = "Niko1221";
    repo = "Strata";
    rev = "1735d6471df29b42c26170efaac1f1446a58640f";
    sha256 = "152c38qnr5g5q631misrh1x9bpc72i0l89mh4v1a6824pyhc0zc2";
  };
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.makeWrapper
    cuda.cuda_nvcc
  ];
  buildInputs = [
    cuda.cuda_cudart
    cuda.libcublas
  ];
  cmakeFlags = [
    "-DSTRATA_ENABLE_CUDA=ON"
    "-DSTRATA_BUILD_TESTS=OFF"
    # Nix strips -march=native. Select Andromeda's CPU features explicitly
    # rather than silently compiling ggml's expert fallback without SIMD.
    "-DSTRATA_PORTABLE=ON"
    "-DGGML_AVX512=ON"
    "-DGGML_AVX512_VBMI=ON"
    "-DGGML_AVX512_VNNI=ON"
    "-DGGML_AVX512_BF16=ON"
    "-DCMAKE_CUDA_ARCHITECTURES=120"
    "-DSTRATA_MMQ_KQUANTS=ON"
    "-DSTRATA_GGML_DIR=${llamaSource}"
  ];
  ninjaFlags = [ "strata" ];
  installPhase = ''
    runHook preInstall
    install -Dm755 strata $out/bin/strata
    mkdir -p $out/share/strata
    cp -r ../serve ../tools ../data $out/share/strata/
    makeWrapper ${python}/bin/python $out/bin/strata-serve \
      --add-flags "-m serve.server" \
      --prefix PYTHONPATH : $out/share/strata \
      --prefix LD_LIBRARY_PATH : /run/opengl-driver/lib
    runHook postInstall
  '';
}
