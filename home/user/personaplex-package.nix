{ pkgs }:
let
  python = pkgs.python3;
  ps = python.pkgs;
  # This runtime is only deployed on Andromeda's RTX 5090. The stock NVSHMEM
  # build targets every GPU generation and nests four compiler threads per job.
  nvshmem = pkgs.cudaPackages.libnvshmem.overrideAttrs (old: {
    cmakeFlags =
      builtins.filter (
        flag:
        !(builtins.any (prefix: pkgs.lib.hasPrefix prefix flag) [
          "-DCMAKE_CUDA_ARCHITECTURES"
          "-DNVSHMEM_BUILD_TESTS"
          "-DNVSHMEM_BUILD_EXAMPLES"
        ])
      ) old.cmakeFlags
      ++ [
        "-DCMAKE_CUDA_ARCHITECTURES:STRING=120"
        "-DNVSHMEM_BUILD_TESTS:BOOL=OFF"
        "-DNVSHMEM_BUILD_EXAMPLES:BOOL=OFF"
        "-DNVCC_THREADS:BOOL=OFF"
      ];
  });
  torch = ps.torch-bin.override {
    cudaPackages = pkgs.cudaPackages // {
      libnvshmem = nvshmem;
    };
  };
  sphn = ps.buildPythonPackage {
    pname = "sphn";
    version = "0.1.12";
    format = "wheel";
    src = pkgs.fetchurl {
      url = "https://files.pythonhosted.org/packages/60/c5/cbf2b9b8b7221723e8314cdd8c3ca4fbfca44ae6680c64c7840c12f21d78/sphn-0.1.12-cp313-cp313-manylinux_2_17_x86_64.manylinux2014_x86_64.whl";
      hash = "sha256:0b44b49017f6edd62a2977241fde54f096c92da2906ec6bd55f18faa6dee4c27";
    };
    nativeBuildInputs = [ pkgs.autoPatchelfHook ];
    buildInputs = [ pkgs.stdenv.cc.cc.lib ];
    dependencies = [ ps.numpy ];
    pythonImportsCheck = [ "sphn" ];
  };
  moshi = ps.buildPythonPackage {
    pname = "moshi-personaplex";
    version = "0-unstable-3428dfd";
    pyproject = true;
    src = pkgs.fetchFromGitHub {
      owner = "NVIDIA";
      repo = "personaplex";
      rev = "3428dfd95309a7f3c84fd93259ded0f810d1ff91";
      hash = "sha256-2QpdK/Cb1ljWJtw8+GH3CGsBxrMQrcWcq4asPLa7rQ8=";
    };
    sourceRoot = "source/moshi";
    build-system = [ ps.setuptools ];
    # Upstream explicitly requires newer Torch on Blackwell, despite its old
    # metadata bounds. Exercise the resulting combination in the GPU smoke test.
    pythonRelaxDeps = true;
    dependencies = with ps; [
      numpy
      safetensors
      huggingface-hub
      einops
      sentencepiece
      sounddevice
      sphn
      torch
      aiohttp
    ];
    postPatch = ''
      substituteInPlace moshi/server.py \
        --replace-fail 'web.run_app(app, port=args.port, ssl_context=ssl_context)' 'web.run_app(app, host=args.host, port=args.port, ssl_context=ssl_context, access_log=None)' \
        --replace-fail 'hf_hub_download(args.hf_repo, "config.json")' 'pass  # Assets are installed and verified separately; no runtime downloads.' \
        --replace-fail 'clog.log("info", f"text prompt: {request.query['"'"'text_prompt'"'"']}")' 'pass  # Do not journal conversation prompts.'
      ${python.interpreter} ${../config/voice/personaplex_patch.py} moshi/server.py ${../config/voice/personaplex-browser-guard.js}
    '';
    pythonImportsCheck = [
      "moshi.models.loaders"
      "sphn"
    ];
  };
in
python.withPackages (_: [ moshi ])
