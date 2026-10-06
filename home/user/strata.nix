{
  config,
  lib,
  pkgs,
  contextSize,
  apiKeyPath,
}:
let
  package = import ./strata-package.nix { inherit pkgs; };
  dataDir = "${config.xdg.dataHome}/strata";
  stateDir = "${config.xdg.stateHome}/strata";
  quant = "UD-Q4_K_XL";
  alias = "qwen3.8-flash-next";
  modelDir = "${dataDir}/models/unsloth-ud-q4_k_xl";
  firstShard = "${modelDir}/Qwen3.8-Flash-Next-${quant}-00001-of-00004.gguf";
  pack = "${dataDir}/packs/unsloth-ud-q4_k_xl";
  mtp = "${dataDir}/mtp";
  runtimeConfig = pkgs.writeText "strata-config.json" (
    builtins.toJSON {
      exe = "${package}/bin/strata";
      cwd = dataDir;
      args = [
        "--pack"
        pack
        "--native"
        firstShard
        "--expert-profile"
        "${package}/share/strata/data/expert-profile.bin"
        "--expert-cache"
        "auto"
        "--resident-budget-gib"
        "80"
        "--prefill"
        "auto"
        "--spec"
        "4"
        "--spec-min-p"
        "0.5"
        "--mtp"
        "${mtp}/rt"
        "--mtp-draft-vocab"
        "${package}/share/strata/data/draft_vocab.bin"
        "--max-context"
        (toString contextSize)
        "--kv"
        "int8"
        "--vram-reserve-mib"
        "2048"
      ];
      tokenizer = "${pack}/tokenizer";
      model_name = alias;
      parallel = 2;
      log = "${stateDir}/engine.log";
      host = "0.0.0.0";
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
      export STRATA_GGUF_PY=${package.llamaSource}/gguf-py
      if [ ! -f ${lib.escapeShellArg "${pack}/native_experts.txt"} ] || [ ! -f ${lib.escapeShellArg "${pack}/tokenizer/vocab.json"} ]; then
        ${package.python}/bin/python ${package}/share/strata/tools/iq_pack.py \
          --gguf ${lib.escapeShellArg firstShard} --out ${lib.escapeShellArg pack} --compat-bf16
      fi
      if [ ! -f ${lib.escapeShellArg "${mtp}/rt/experts.bin"} ]; then
        ${package.python}/bin/python ${package}/share/strata/tools/mtp_fetch.py fetch --out ${lib.escapeShellArg mtp}
        ${package.python}/bin/python ${package}/share/strata/tools/mtp_pack.py \
          --src ${lib.escapeShellArg mtp} --experts q2_0 --out ${lib.escapeShellArg "${mtp}/mtp-q2_0.gguf"}
        ${package.python}/bin/python ${package}/share/strata/tools/mtp_rt.py \
          --gguf ${lib.escapeShellArg "${mtp}/mtp-q2_0.gguf"} --out ${lib.escapeShellArg "${mtp}/rt"}
      fi
      echo 'Strata model prepared. Start local-llm.service to load it.'
    '';
  };
  server = pkgs.writeShellApplication {
    name = "strata-server";
    runtimeInputs = [ pkgs.coreutils ];
    text = ''
      umask 077
      if [ ! -s ${lib.escapeShellArg apiKeyPath} ]; then
        echo 'Local model API key missing; refusing to expose Strata.' >&2
        exit 1
      fi
      export STRATA_API_KEY
      STRATA_API_KEY="$(< ${lib.escapeShellArg apiKeyPath})"
      mkdir -p ${lib.escapeShellArg stateDir}
      exec ${package}/bin/strata-serve --engine strata --port 8081 --config ${runtimeConfig} "$@"
    '';
  };
in
{
  inherit
    package
    fetch
    server
    alias
    quant
    ;
  readyPath = "${mtp}/rt/experts.bin";
}
