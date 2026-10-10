{
  config,
  lib,
  pkgs,
  contextSize,
}:
let
  package = import ./strata-package.nix { inherit pkgs; };
  dataDir = "${config.xdg.dataHome}/strata";
  quant = "UD-Q4_K_XL";
  alias = "qwen3.8-flash-next";
  modelDir = "${dataDir}/models/unsloth-ud-q4_k_xl";
  mtp = "${dataDir}/mtp";
  runtimeConfig = pkgs.writeText "strata-config.json" (
    builtins.toJSON {
      exe = "${package}/bin/strata";
      cwd = "/models/strata";
      args = [
        "--pack"
        "/models/strata/packs/unsloth-ud-q4_k_xl"
        "--native"
        "/models/strata/models/unsloth-ud-q4_k_xl/Qwen3.8-Flash-Next-${quant}-00001-of-00004.gguf"
        "--expert-profile"
        "${package}/share/strata/data/expert-profile.bin"
        "--expert-cache"
        "auto"
        "--resident-budget-gib"
        "80"
        "--prefill"
        "auto"
        # --batch-mtp kills this pack: "mtp: unsupported native MMVQ GGML type".
        "--spec"
        "4"
        "--spec-min-p"
        "0.5"
        "--mtp"
        "/models/strata/mtp/rt"
        "--mtp-draft-vocab"
        "${package}/share/strata/data/draft_vocab.bin"
        "--max-context"
        (toString contextSize)
        "--kv"
        "int8"
        "--vram-reserve-mib"
        "2048"
      ];
      tokenizer = "/models/strata/packs/unsloth-ud-q4_k_xl/tokenizer";
      model_name = alias;
      # Shorten max_tokens to the room left instead of answering 400 (#545).
      fit_max_tokens = true;
      parallel = 2;
      log = "/tmp/engine.log";
      host = "0.0.0.0";
      # The authenticated gateway reaches the private engine by its Docker alias.
      allowed_hosts = [ "engine" ];
      port = 8081;
      open_browser = false;
    }
  );
  shards = [
    {
      number = "00001";
      sha256 = "4448186216b3af4cc558bbce2c3213f01608f8f8b2e5267a9767971dd3ec8082";
    }
    {
      number = "00002";
      sha256 = "3f342f1c1580473f1ee94ddd5b28206e8c07a70fa1a366f59d1d6c922919a6c9";
    }
    {
      number = "00003";
      sha256 = "56758f40269cad5cd9b0d3d6fbae0f40f6d5be6de49e4ab392dbe83157d9cbd3";
    }
    {
      number = "00004";
      sha256 = "753bda48b98ba4f1636134a90a967de1b2d3908a236c026e464777342e53510a";
    }
  ];
  fetch = pkgs.writeShellApplication {
    name = "strata-fetch";
    runtimeInputs = [
      pkgs.coreutils
      pkgs.curl
      pkgs.docker_29
    ];
    text = ''
      umask 077
      mkdir -p ${lib.escapeShellArg modelDir}
      ${lib.concatMapStringsSep "\n" (
        shard:
        let
          name = "Qwen3.8-Flash-Next-${quant}-${shard.number}-of-00004.gguf";
          path = "${modelDir}/${name}";
        in
        ''
          if [ ! -f ${lib.escapeShellArg path} ]; then
            curl --fail --location --retry 3 --continue-at - \
              --output ${lib.escapeShellArg "${path}.partial"} \
              'https://huggingface.co/unsloth/Qwen3.8-Flash-Next-GGUF/resolve/38bb39ee97821de2c9009abb7e93950eec396e66/${quant}/${name}'
            printf '%s  %s\n' '${shard.sha256}' ${lib.escapeShellArg "${path}.partial"} | sha256sum --check
            mv ${lib.escapeShellArg "${path}.partial"} ${lib.escapeShellArg path}
          else
            printf '%s  %s\n' '${shard.sha256}' ${lib.escapeShellArg path} | sha256sum --check
          fi
        ''
      ) shards}
      image_id="$(< ${imageId})"
      docker image inspect "$image_id" >/dev/null 2>&1 || docker load --input ${image} >/dev/null
      docker run --rm --network bridge --security-opt no-new-privileges --cap-drop ALL \
        --user "$(id -u):$(id -g)" --env HOME=/tmp \
        --volume ${lib.escapeShellArg "${dataDir}:/models/strata:rw"} \
        --entrypoint ${prepare}/bin/strata-prepare "$image_id"
      echo 'Strata model prepared. Start it from Local AI.'
    '';
  };
  prepare = pkgs.writeShellApplication {
    name = "strata-prepare";
    text = ''
      export STRATA_GGUF_PY=${package.llamaSource}/gguf-py
      cd /models/strata
      if [[ ! -f packs/unsloth-ud-q4_k_xl/native_experts.txt || ! -f packs/unsloth-ud-q4_k_xl/tokenizer/vocab.json ]]; then
        ${package.python}/bin/python ${package}/share/strata/tools/iq_pack.py \
          --gguf models/unsloth-ud-q4_k_xl/Qwen3.8-Flash-Next-${quant}-00001-of-00004.gguf \
          --out packs/unsloth-ud-q4_k_xl --compat-bf16
      fi
      if [[ ! -f mtp/rt/experts.bin ]]; then
        ${package.python}/bin/python ${package}/share/strata/tools/mtp_fetch.py fetch --out mtp
        ${package.python}/bin/python ${package}/share/strata/tools/mtp_pack.py \
          --src mtp --experts q2_0 --out mtp/mtp-q2_0.gguf
        ${package.python}/bin/python ${package}/share/strata/tools/mtp_rt.py \
          --gguf mtp/mtp-q2_0.gguf --out mtp/rt
      fi
    '';
  };
  server = pkgs.writeShellApplication {
    name = "strata-container-server";
    text = ''
      export LD_LIBRARY_PATH="''${NVIDIA_CTK_LIBCUDA_DIR:-/run/opengl-driver/lib}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
      exec ${package}/bin/strata-serve --engine strata --port 8081 --config ${runtimeConfig} "$@"
    '';
  };
  image = pkgs.dockerTools.buildLayeredImage {
    name = "strata-andromeda";
    contents = [
      server
      prepare
      pkgs.glibc.bin
      pkgs.cacert
    ];
    extraCommands = ''
      mkdir -p tmp models
      chmod 1777 tmp
    '';
    config = {
      Entrypoint = [ "${server}/bin/strata-container-server" ];
      Env = [
        "HOME=/tmp"
        "SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
      ];
    };
  };
  imageId = pkgs.runCommand "strata-image-id" { nativeBuildInputs = [ pkgs.jq ]; } ''
    tar -xOf ${image} manifest.json | jq -er '.[0].Config | "sha256:" + (split("/")[-1] | sub("\\.json$"; ""))' > "$out"
    grep -Eq '^sha256:[0-9a-f]{64}$' "$out"
  '';
in
{
  inherit
    package
    fetch
    image
    imageId
    alias
    quant
    dataDir
    ;
  readyPath = "${mtp}/rt/experts.bin";
}
