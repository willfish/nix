{
  config,
  lib,
  pkgs,
  hostName,
  readSopsSecret,
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
  voicePython = pkgs.python3;
  osdPython = pkgs.python3.withPackages (ps: [
    ps.pygobject3
    ps.pycairo
  ]);
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
        pkgs.systemd
        pkgs.herdr
      ];
      text = ''
        export PATH="${config.home.homeDirectory}/.local/bin:$PATH"
        export PI_VOICE_COMMAND="$0"
        export AGENT_VOICE_LAUNCH_KIND=${lib.escapeShellArg harness}
        secret=${lib.escapeShellArg config.sops.secrets.DEEPGRAM_API_KEY.path}
        if [ -r "$secret" ]; then
          DEEPGRAM_API_KEY="$(${readSopsSecret}/bin/read-sops-secret "$secret")"
          export DEEPGRAM_API_KEY
        fi
        exec python3 ${voiceScripts}/voice_controller.py "$@"
      '';
    };
  voice = makeVoice "pi";
  voiceInteract = pkgs.writeShellApplication {
    name = "pi-voice-interact";
    runtimeInputs = [ pkgs.systemd ];
    text = ''
      systemctl --user start --no-block pi-voice-osd.service || true
      exec ${voice}/bin/pi-voice interact "$@"
    '';
  };
  voiceOsd = pkgs.stdenv.mkDerivation {
    pname = "pi-voice-osd";
    version = "1";
    src = ../config/voice;
    dontUnpack = true;
    dontBuild = true;
    nativeBuildInputs = [
      pkgs.wrapGAppsHook4
      pkgs.gobject-introspection
    ];
    buildInputs = [
      pkgs.gtk4
      pkgs.gtk4-layer-shell
    ];
    dontWrapGApps = true;
    installPhase = "mkdir -p $out/bin";
    preFixup = ''
      makeWrapper ${osdPython}/bin/python3 $out/bin/pi-voice-osd \
        --add-flags ${voiceScripts}/voice_osd.py \
        --prefix PATH : ${
          lib.makeBinPath [
            pkgs.hyprland
            voice
          ]
        } \
        ''${gappsWrapperArgs[@]} \
        --set GDK_BACKEND wayland \
        --set LD_PRELOAD ${pkgs.gtk4-layer-shell}/lib/libgtk4-layer-shell.so
    '';
  };
  desktopSettings = import ../config/hyprland/settings.nix;
  # Read the active style each time Fuzzel opens, not at build time.
  menuConfig = pkgs.writeText "voice-menu-fuzzel.ini" ''
    include=${config.xdg.stateHome}/theme-menu/active/fuzzel.ini
    anchor=${desktopSettings.launcher.anchor}
    layer=${desktopSettings.launcher.layer}
    width=${toString desktopSettings.menus.voice.width}
    lines=${toString desktopSettings.menus.voice.lines}
    minimal-lines=yes
    match-mode=${desktopSettings.launcher.matchMode}
  '';
  voiceMenu = pkgs.writeShellApplication {
    name = "voice-menu";
    runtimeInputs = [
      voicePython
      pkgs.fuzzel
      pkgs.systemd
    ];
    text = ''
      export PI_PERSONAPLEX_ENABLED=${if hostName == "andromeda" then "1" else "0"}
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
  imports = [ ./personaplex.nix ];
  config = lib.mkIf voiceStt {
    home.packages = [
      voice
      voiceInteract
      voiceOsd
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
      Unit.Description = "Agent voice hotkeys and selected session";
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
    systemd.user.services.pi-voice-osd = {
      Unit = {
        Description = "Voice status card for the selected Pi session";
        After = [
          "graphical-session.target"
          "pi-voice.service"
        ];
        PartOf = [ "graphical-session.target" ];
      };
      Service = {
        ExecStart = "${voiceOsd}/bin/pi-voice-osd";
        Restart = "on-failure";
        RestartSec = 2;
      };
      Install.WantedBy = [ "graphical-session.target" ];
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
