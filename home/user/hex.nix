{
  config,
  pkgs,
  lib,
  hostName,
  ...
}:
let
  voice = import ./voice-supported.nix { inherit pkgs hostName; };
  cpuRewrite = voice.stt && hostName == "andromeda";
  rewriteModel = pkgs.fetchurl {
    url = "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/7dabda4d13d513e3e842b20f0d435c732f172cbe/qwen2.5-3b-instruct-q4_k_m.gguf";
    hash = "sha256:626b4a6678b86442240e33df819e00132d3ba7dddfe1cdc4fbb18e0a9615c62d";
  };
  rewriteLlama = pkgs.llama-cpp.override {
    cudaSupport = false;
    rocmSupport = false;
    vulkanSupport = false;
  };
in
{
  programs.hex = lib.mkIf voice.stt {
    enable = true;
    package = pkgs.hex-voice;
    autostart = true;
    installModel = true;
    language = "en";
    settings = {
      shortcut_mode = "toggle";
      paste_with_shift = true;
    }
    // lib.optionalAttrs cpuRewrite {
      "dictation_processing.default_mode" = {
        name = "Global";
        applications = [ ];
        browser_hosts = [ ];
        replacements = [ ];
        transformations = [ ];
        post_processing = {
          enabled = true;
          model = "hex-rewrite/qwen2.5-3b-instruct";
          variant = null;
          deadline_seconds = 30;
          prompt = "Rewrite only the transcript into Shakespearean English. Preserve every action, question, name, number and technical term; never change what the speaker asks or means. Keep technical verbs such as restart unchanged. Use thou, thee, thy, prithee and Early Modern grammar where natural. Do not answer the transcript or add information. Output only the rewritten transcript. Examples: Thanks. -> I thank thee. Please save the file. -> Prithee, save the file.";
        };
      };
    };
  };
  # Leave Andromeda's VRAM for local models: CPU transcription and Lavapipe UI rendering.
  systemd.user.services.hex = lib.mkIf (voice.stt && hostName == "andromeda") {
    Service.Environment = [
      "TRANSCRIBE_BACKENDS=cpu"
      "VK_DRIVER_FILES=${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.x86_64.json"
    ];
  };
  systemd.user.services.hex-rewrite = lib.mkIf cpuRewrite {
    Unit.Description = "CPU-only dictation rewriting model";
    Service = {
      ExecStart = lib.concatStringsSep " " [
        "${rewriteLlama}/bin/llama-server"
        "--model ${rewriteModel} --alias qwen2.5-3b-instruct"
        "--host 127.0.0.1 --port 18083 --no-webui"
        "--device none --n-gpu-layers 0 --no-kv-offload --no-op-offload --fit off"
        "--ctx-size 8192 --parallel 1 --n-predict 1024"
        "--threads 8 --threads-batch 16 --poll 0"
        "--jinja --reasoning off --temp 0.2 --top-p 0.9 --min-p 0"
      ];
      Restart = "on-failure";
      RestartSec = 5;
      Nice = 5;
      UMask = "0077";
    };
    Install.WantedBy = [ "default.target" ];
  };
  # The running portal may retain the application dirs from before a profile
  # switch. Publish its app ID in the stable user directory as well.
  xdg.dataFile."applications/anomaly.hex.desktop" = lib.mkIf voice.stt {
    source = "${config.programs.hex.package}/share/applications/anomaly.hex.desktop";
  };
}
