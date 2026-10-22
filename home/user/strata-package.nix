{ pkgs, source }:
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
  # flake.lock owns the revision, so the version follows the locked commit date.
  d = source.lastModifiedDate;
  version = "0-unstable-${builtins.substring 0 4 d}-${builtins.substring 4 2 d}-${builtins.substring 6 2 d}";
in
pkgs.stdenv.mkDerivation {
  pname = "strata";
  passthru = { inherit python llamaSource; };
  inherit version;
  src = source;
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
    cp -r ../serve ../tools ../strata_control ../strata_cli ../data ../setup.py $out/share/strata/
    makeWrapper ${python}/bin/python $out/bin/strata-serve \
      --add-flags "-m serve.server" \
      --prefix PYTHONPATH : $out/share/strata \
      --prefix LD_LIBRARY_PATH : /run/opengl-driver/lib
    runHook postInstall
  '';
}
