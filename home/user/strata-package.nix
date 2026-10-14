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
  version = "0.1.43.1";
  src = pkgs.fetchFromGitHub {
    owner = "Niko1221";
    repo = "Strata";
    rev = "a0116b2782a5b82622358c04a06496485d12b2d6"; # v0.1.43.1
    sha256 = "185604whgpxqimr6xpf13yllvqy65w7izq01qqb2vf9431w6rqm2";
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
  # setup.py supplies calibration's CPU-topology helpers. Before executing scripts
  # or upgrading this package, follow the security-review note in strata.nix.
  installPhase = ''
    runHook preInstall
    install -Dm755 strata $out/bin/strata
    mkdir -p $out/share/strata
    cp -r ../serve ../tools ../data ../setup.py $out/share/strata/
    makeWrapper ${python}/bin/python $out/bin/strata-serve \
      --add-flags "-m serve.server" \
      --prefix PYTHONPATH : $out/share/strata \
      --prefix LD_LIBRARY_PATH : /run/opengl-driver/lib
    runHook postInstall
  '';
}
