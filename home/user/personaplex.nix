{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  enabled = hostName == "andromeda" && pkgs.stdenv.isLinux;
  runtime = import ./personaplex-package.nix { inherit pkgs; };
  dataDir = "${config.home.homeDirectory}/.local/share/pi-voice/personaplex";
  models = pkgs.writeShellApplication {
    name = "personaplex-models";
    runtimeInputs = [ pkgs.python3 ];
    text = ''
      umask 077
      exec python3 ${../config/voice/personaplex_models.py} --data-dir ${lib.escapeShellArg dataDir} "$@"
    '';
  };
  memoryCheck = pkgs.writeShellApplication {
    name = "personaplex-memory-check";
    runtimeInputs = [ pkgs.gawk ];
    text = ''
      # Do not silently evict a running coding model to make room for speech.
      /run/current-system/sw/bin/nvidia-smi --id=0 --query-gpu=memory.free --format=csv,noheader,nounits |
        awk '$1 >= 24576 { ok=1 } END { if (!ok) { print "PersonaPlex needs at least 24 GiB free GPU memory; unload other models first." > "/dev/stderr"; exit 1 } }'
    '';
  };
  open = pkgs.writeShellApplication {
    name = "personaplex-open";
    runtimeInputs = [
      pkgs.python3
      pkgs.systemd
      pkgs.xdg-utils
    ];
    text = ''
      exec python3 ${../config/voice}/voice_conversation.py
    '';
  };
in
{
  config = lib.mkIf enabled {
    home.packages = [ models ];
    systemd.user.services.personaplex = {
      Unit = {
        Description = "Experimental local PersonaPlex conversation";
        Conflicts = [
          "pi-voice.service"
          "pi-voice-stt.service"
          "pi-voice-tts.service"
        ];
        After = [
          "pi-voice.service"
          "pi-voice-stt.service"
          "pi-voice-tts.service"
        ];
        PartOf = [ "graphical-session.target" ];
      };
      Service = {
        Type = "simple";
        ExecStartPre = [
          "${memoryCheck}/bin/personaplex-memory-check"
          "${models}/bin/personaplex-models --check-only"
        ];
        ExecStart = "${runtime}/bin/python3 -m moshi.server --host 127.0.0.1 --port 8998 --device cuda --static ${dataDir}/dist --voice-prompt-dir ${dataDir}/voices --moshi-weight ${dataDir}/model.safetensors --mimi-weight ${dataDir}/tokenizer-e351c8d8-checkpoint125.safetensors --tokenizer ${dataDir}/tokenizer_spm_32k_3.model";
        Environment = [
          "HF_HUB_OFFLINE=1"
          "HF_HUB_DISABLE_TELEMETRY=1"
          # Keep startup bounded and avoid invoking a host compiler at runtime.
          # Upstream CUDA graph acceleration remains enabled.
          "NO_TORCH_COMPILE=1"
        ];
        UMask = "0077";
        NoNewPrivileges = true;
        PrivateTmp = true;
        TimeoutStartSec = 300;
        TimeoutStopSec = 20;
        KillMode = "control-group";
        Restart = "no";
        MemoryMax = "48G";
      };
    };
    systemd.user.services.personaplex-open = {
      Unit = {
        Description = "Open PersonaPlex when the local model is ready";
        After = [ "personaplex.service" ];
        PartOf = [ "personaplex.service" ];
      };
      Service = {
        Type = "oneshot";
        ExecStart = "${open}/bin/personaplex-open";
        TimeoutStartSec = 300;
      };
    };
  };
}
