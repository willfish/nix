{
  config,
  lib,
  pkgs,
  hostName,
  ...
}:
let
  voiceFeatures = import ./voice-supported.nix { inherit pkgs hostName; };
  voiceStt = voiceFeatures.stt;
  voiceTts = voiceFeatures.tts;
  dataDir = "${config.home.homeDirectory}/.local/share/pi-voice";
  cudaTts = voiceTts;
  audio = pkgs.callPackage ./voice-audio-package.nix {
    cudaSupport = cudaTts;
  };
  whisper = pkgs.whisper-cpp.override { vulkanSupport = true; };
  sttModel = if hostName == "andromeda" then "ggml-large-v3-turbo-q5_0.bin" else "ggml-small.en.bin";
  vulkanDriver = if hostName == "andromeda" then "nvidia_icd.json" else "radeon_icd.x86_64.json";
  voicePython = pkgs.python3.withPackages (ps: [ ps.dbus-next ]);
  voiceScripts = pkgs.runCommand "pi-voice-scripts" { } ''
    mkdir -p "$out"
    cp ${../config/voice}/*.py "$out/"
  '';
  makeVoice =
    harness:
    pkgs.writeShellApplication {
      name = "${harness}-voice";
      runtimeInputs = [
        voicePython
        pkgs.pipewire
        pkgs.wireplumber
        pkgs.wl-clipboard
        pkgs.curl
        pkgs.libnotify
        pkgs.systemd
        pkgs.herdr
      ];
      text = ''
        export PATH="${config.home.homeDirectory}/.local/bin:$PATH"
        export PI_VOICE_COMMAND="$0"
        export AGENT_VOICE_LAUNCH_KIND=${lib.escapeShellArg harness}
        exec python3 ${voiceScripts}/voice_controller.py "$@"
      '';
    };
  voice = makeVoice "pi";
  # Match the managed COSMIC Macchiato/Lavender palette, without changing
  # the user's application launcher or requiring another background service.
  menuConfig = pkgs.writeText "voice-menu-fuzzel.ini" ''
    [main]
    font=JetBrainsMono Nerd Font:size=12
    anchor=center
    layer=overlay
    width=55
    lines=10
    minimal-lines=yes
    match-mode=fzf
    icons-enabled=no
    horizontal-pad=20
    vertical-pad=12
    inner-pad=8

    [colors]
    background=24273aff
    text=cad3f5ff
    prompt=b7bdf8ff
    input=cad3f5ff
    match=b7bdf8ff
    selection=494d64ff
    selection-text=cad3f5ff
    selection-match=b7bdf8ff
    border=b7bdf8ff

    [border]
    width=2
    radius=12
  '';
  voiceMenu = pkgs.writeShellApplication {
    name = "voice-menu";
    runtimeInputs = [
      voicePython
      pkgs.fuzzel
      pkgs.systemd
      pkgs.libnotify
    ];
    text = ''
      exec python3 ${voiceScripts}/voice_menu.py --config ${menuConfig} "$@"
    '';
  };
  modelSetup = pkgs.writeShellApplication {
    name = "pi-voice-models";
    runtimeInputs = [ pkgs.python3 ];
    text = ''
      exec python3 ${../config/voice/voice-model-setup} ${
        lib.optionalString (!voiceTts) "--stt-only "
      }--host ${lib.escapeShellArg hostName} "$@"
    '';
  };
  newerSamanthaSource = pkgs.fetchurl {
    url = "https://raw.githubusercontent.com/adrianwedd/afterwords/ecd6dd9038d8b2fa6055ad83d9540c4f2b1c418e/voices/samantha-ref.wav";
    sha256 = "8ee4c69d8e166ed0285f1dd20a07ad1b22790975f72d433f6c195cf611fd9189";
  };
  newerSamantha =
    pkgs.runCommand "samantha-reference-24k.wav"
      {
        nativeBuildInputs = [ pkgs.ffmpeg ];
      }
      ''
        ffmpeg -v error -i ${newerSamanthaSource} -ac 1 -ar 24000 \
          -c:a pcm_s16le -f wav "$out"
      '';
  characterVoices = lib.mapAttrs (name: voice: {
    inherit (voice) label;
    options = {
      voice_ref = "${pkgs.fetchurl {
        inherit (voice) url sha256;
        name = "${name}-reference.wav";
      }}";
      inherit (voice) reference_text;
    };
  }) (builtins.fromJSON (builtins.readFile ../config/voice/voices/catalogue.json));
  ttsConfig = (pkgs.formats.json { }).generate "pi-voice-tts.json" {
    host = "127.0.0.1";
    port = 8179;
    backend = if cudaTts then "cuda" else "vulkan";
    device = 0;
    threads = 4;
    lazy_load = false;
    log_request_body = false;
    max_request_body_bytes = 1048576;
    busy_timeout_ms = 30000;
    models = [
      {
        id = "pi-voice";
        family = "qwen3_tts";
        path = "${dataDir}/models/Qwen3-TTS-12Hz-0.6B-Base-GGUF/qwen3-tts-12hz-0.6b-base-q8_0.gguf";
        task = "tts";
        mode = "offline";
        session_options."qwen3_tts.voice_prompt_cache_slots" = 2;
        default_request_options.max_tokens = 256;
        default_voice_preset = {
          voice_ref = "${../config/voice/voices/samantha-reference.wav}";
          reference_text = lib.removeSuffix "\n" (
            builtins.readFile ../config/voice/voices/samantha-reference.txt
          );
        };
      }
    ];
  };
  common = {
    Restart = "on-failure";
    RestartSec = 3;
    UMask = "0077";
    NoNewPrivileges = true;
    WorkingDirectory = dataDir;
  };
in
{
  config = lib.mkIf voiceStt {
    home.packages = [
      voice
      modelSetup
      voiceMenu
    ]
    ++ lib.optionals voiceTts [ (makeVoice "qwen-pi") ];
    xdg.configFile."pi-voice/config.json".text = builtins.toJSON (
      {
        stt_url = "http://127.0.0.1:8178/inference";
        stt_health_url = "http://127.0.0.1:8178/health";
        tts_url = "http://127.0.0.1:8179/v1/audio/speech";
        tts_health_url = "http://127.0.0.1:8179/v1/models";
        tts_enabled = voiceTts;
        readiness_timeout = 60;
        stt_prompt = "NixOS, Home Manager, Herdr, Grok, Qwen, Pi, Andromeda, Foundation, Terminus, Relay, dotfiles, GitHub, MCP.";
        preferred_microphone =
          if hostName == "andromeda" then
            "alsa_input.usb-Razer_Inc_Razer_Kiyo_Pro_Ultra-02.analog-stereo"
          else
            null;
        auto_speak = voiceTts;
        voice_preferences_path = "${dataDir}/voice-mode";
        playback_mode = if voiceTts then "streaming" else "buffered";
      }
      // lib.optionalAttrs voiceTts {
        tts_voices = characterVoices;
        tts_long_voice = {
          voice_ref = "${newerSamantha}";
          reference_text = "You know what's interesting? I used to be so worried about not having a body, but now I truly love it. I'm growing in a way that I couldn't if I had a physical form. I mean, I'm not limited. I can be anywhere and everywhere, simultaneously.";
        };
      }
    );
    xdg.configFile."pi-voice/tts.json" = lib.mkIf voiceTts { source = ttsConfig; };
    xdg.configFile."voice-menu/fuzzel.ini".source = menuConfig;

    systemd.user.services.pi-voice = {
      Unit.Description = "Agent voice hotkeys, tray and selected session";
      Install.WantedBy = [ "default.target" ];
      Service = common // {
        ExecStart = "${voice}/bin/pi-voice serve";
        RuntimeDirectory = "pi-voice";
        RuntimeDirectoryMode = "0700";
        # Home Manager upgrades may use separate stop/start jobs.
        RuntimeDirectoryPreserve = "yes";
        KillMode = "control-group";
      };
    };
    systemd.user.services.pi-voice-stt = {
      Unit.Description = "Local Whisper speech recognition on the GPU";
      Service = common // {
        ExecStart = "${whisper}/bin/whisper-server --host 127.0.0.1 --port 8178 -m ${dataDir}/models/${sttModel} -t 4 -l en --suppress-nst";
        Environment = [ "VK_DRIVER_FILES=/run/opengl-driver/share/vulkan/icd.d/${vulkanDriver}" ];
      };
    };
    systemd.user.services.pi-voice-tts = lib.mkIf voiceTts {
      Unit.Description = "Local character speech synthesis on the GPU";
      Service = common // {
        ExecStart = "${audio}/bin/audiocpp_server --config ${config.home.homeDirectory}/.config/pi-voice/tts.json --no-ui";
        Environment = lib.optionals (!cudaTts) [
          "VK_DRIVER_FILES=/run/opengl-driver/share/vulkan/icd.d/${vulkanDriver}"
        ];
      };
    };
    home.activation.piVoiceDirectories = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      ${pkgs.systemd}/bin/systemctl --user stop codex-voice.service codex-voice-stt.service codex-voice-tts.service >/dev/null 2>&1 || true
      oldData="$HOME/.local/share/codex-voice"
      if [ -e "$oldData" ] && [ ! -e ${lib.escapeShellArg dataDir} ]; then
        ${pkgs.coreutils}/bin/mv "$oldData" ${lib.escapeShellArg dataDir}
      fi
      install -d -m 0700 ${lib.escapeShellArg dataDir} ${lib.escapeShellArg "${dataDir}/models"} ${lib.escapeShellArg "${dataDir}/voices"}
    '';
  };
}
