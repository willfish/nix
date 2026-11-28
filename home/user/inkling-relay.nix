{
  config,
  lib,
  pkgs,
  modelRoot,
}:
let
  data = "${config.xdg.dataHome}/inkling-relay";
  model = "${modelRoot}/Inkling/mlx-4bit";
  environment = "${data}/mlx-vlm-0.8.1";
  keyFile = "${config.xdg.configHome}/local-llm/api-key";
  repository = "VisualInference/Inkling-2-Mini-MLX-4bit";
  revision = "443e30e1ddbb05b711975a6b549ffe209755edc6";
  files = {
    "chat_template.jinja" = 6868;
    "config.json" = 111256;
    "generation_config.json" = 98;
    "model-00001-of-00003.safetensors" = 5352372240;
    "model-00002-of-00003.safetensors" = 5241506160;
    "model-00003-of-00003.safetensors" = 5253054939;
    "model.safetensors.index.json" = 182747;
    "processor_config.json" = 461;
    "special_tokens_map.json" = 529;
    "tiktoken/tokenizer.model" = 3386494;
    "tokenizer.json" = 26178678;
    "tokenizer_config.json" = 13235;
  };
  checkModel = pkgs.writeShellScript "inkling-relay-check-model" ''
    set -euo pipefail
    cd ${lib.escapeShellArg model}
    check_file() {
      local file=$1 size=$2 commit metadata=".cache/huggingface/download/$1.metadata"
      test -f "$file" && test "$(/usr/bin/stat -f %z "$file")" = "$size" &&
        read -r commit < "$metadata" && test "$commit" = ${revision}
    }
    ${lib.concatStringsSep "\n" (
      lib.mapAttrsToList (file: size: "check_file ${lib.escapeShellArg file} ${toString size}") files
    )}
    test ! -e mtp.safetensors
  '';
  requirements = ../../programs/inkling-relay/requirements.txt;
  pythonVersion = "3.12.13";
  fingerprint = "${pythonVersion}:${builtins.hashFile "sha256" requirements}";
  runner = pkgs.writeShellScript "inkling-relay-mlx" ''
    set -euo pipefail
    # Fail before binding the MLX port if the installed engine ignores the
    # startup limits. The gateway checks the loaded engine before exposing it.
    exec ${lib.escapeShellArg "${environment}/bin/python"} -I -c '
    import sys
    from importlib.metadata import version
    from mlx_vlm.server import main
    from mlx_vlm.server.generation import (
        get_configured_context_limit, get_max_num_seqs, get_prefill_step_size,
    )
    if version("mlx-vlm") != "0.8.1" or (
        get_configured_context_limit(), get_max_num_seqs(), get_prefill_step_size()
    ) != (65536, 1, 1024):
        sys.exit("Unexpected MLX version or engine limits")
    print("MLX startup settings: context=65536 sequences=1 prefill=1024", flush=True)
    main()
    ' --host 127.0.0.1 --port 18081 --model ${lib.escapeShellArg model} --max-kv-size 65536
  '';
in
{
  setup = pkgs.writeShellApplication {
    name = "inkling-relay-setup";
    runtimeInputs = [ pkgs.uv ];
    text = ''
      umask 077
      test -d ${lib.escapeShellArg modelRoot} || { echo 'Mount the Expansion model SSD first.' >&2; exit 1; }
      export UV_PYTHON_INSTALL_DIR=${lib.escapeShellArg "${data}/python"}
      environment=${lib.escapeShellArg environment}
      if [ -f "$environment/.prepared" ] &&
         [ "$(< "$environment/.prepared")" = ${lib.escapeShellArg fingerprint} ] &&
         ${checkModel} 2>/dev/null; then
        exit 0
      fi
      mkdir -p ${lib.escapeShellArg data}
      rm -f "$environment/.prepared"
      uv --no-config venv --allow-existing --managed-python --python ${pythonVersion} "$environment"
      uv --no-config pip sync --python "$environment/bin/python" --only-binary :all: ${requirements}

      if ! ${checkModel} 2>/dev/null; then
        if [ -e ${lib.escapeShellArg "${model}/mtp.safetensors"} ]; then
          echo 'Move mtp.safetensors out of mlx-4bit before preparing the text server.' >&2
          exit 1
        fi
        "$environment/bin/hf" download ${repository} --revision ${revision} \
          --local-dir ${lib.escapeShellArg model} ${lib.escapeShellArgs (builtins.attrNames files)}
        ${checkModel}
      fi
      printf '%s\n' ${lib.escapeShellArg fingerprint} > "$environment/.prepared"
    '';
  };
  server = pkgs.writeShellApplication {
    name = "inkling-relay-server";
    text = ''
      umask 077
      prepared=${lib.escapeShellArg "${environment}/.prepared"}
      if [ ! -f "$prepared" ] || [ "$(< "$prepared")" != ${lib.escapeShellArg fingerprint} ] ||
         ! ${checkModel}; then
        echo 'Run inkling-relay-setup before starting Inkling.' >&2
        exit 1
      fi
      exec /usr/bin/env -i HOME="$HOME" PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        ${pkgs.nodejs_24}/bin/node ${../../programs/inkling-relay/server.ts} \
        ${lib.escapeShellArgs [
          runner
          model
          keyFile
        ]}
    '';
  };
}
