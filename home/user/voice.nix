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
  dataDir = "${config.home.homeDirectory}/.local/share/pi-voice";
  nativeVoice = import ../../programs/pi-voice-client { inherit pkgs; };
  makeVoice =
    harness:
    pkgs.writeShellApplication {
      name = "${harness}-voice";
      runtimeInputs = [
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
        exec ${nativeVoice}/bin/pi-voice-c "$@"
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
  voiceOsd = pkgs.writeShellApplication {
    name = "pi-voice-osd";
    runtimeInputs = [
      pkgs.hyprland
      pkgs.systemd
      voice
    ];
    text = ''
      export GDK_BACKEND=wayland
      exec ${nativeVoice}/bin/pi-voice-osd-c "$@"
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
      pkgs.fuzzel
      pkgs.systemd
      pkgs.xdg-utils
    ];
    text = ''
      exec ${nativeVoice}/bin/voice-menu-c --config ${menuConfig} "$@"
    '';
  };
  deepgramVoices = [
    "amalthea"
    "andromeda"
    "apollo"
    "arcas"
    "aries"
    "asteria"
    "athena"
    "atlas"
    "aurora"
    "callista"
    "cora"
    "cordelia"
    "delia"
    "draco"
    "electra"
    "harmonia"
    "helena"
    "hera"
    "hermes"
    "hyperion"
    "iris"
    "janus"
    "juno"
    "jupiter"
    "luna"
    "mars"
    "minerva"
    "neptune"
    "odysseus"
    "ophelia"
    "orion"
    "orpheus"
    "pandora"
    "phoebe"
    "pluto"
    "saturn"
    "selene"
    "thalia"
    "theia"
    "vesta"
    "zeus"
  ];
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
      voiceInteract
      voiceOsd
      voiceMenu
    ];
    xdg.configFile."pi-voice/config.json".text = builtins.toJSON {
      backends = [
        {
          id = "deepgram";
          label = "Deepgram";
          listen_url = "https://api.deepgram.com/v1/listen?smart_format=true&punctuate=true";
          speak_url = "https://api.deepgram.com/v1/speak";
          auth_env = "DEEPGRAM_API_KEY";
          auth_scheme = "Token";
          listen_model = "nova-3";
          streaming_wav = true;
          query.mip_opt_out = "true";
          default_voice = "thalia";
          voice_preferences_path = "${dataDir}/deepgram-voice";
          voices = map (name: {
            id = name;
            label = lib.toUpper (builtins.substring 0 1 name) + builtins.substring 1 (-1) name;
            model = "aura-2-${name}-en";
          }) deepgramVoices;
        }
      ];
      stt_prompt = "NixOS, Home Manager, Herdr, Grok, Qwen, Pi, Andromeda, Foundation, Terminus, Relay, dotfiles, GitHub, MCP.";
      preferred_microphone =
        if hostName == "andromeda" then
          "alsa_input.usb-Razer_Inc_Razer_Kiyo_Pro_Ultra-02.analog-stereo"
        else
          null;
      auto_speak = true;
      voice_preferences_path = "${dataDir}/voice-mode";
      playback_mode = "streaming";
    };
    xdg.configFile."voice-menu/fuzzel.ini".source = menuConfig;

    systemd.user.services.pi-voice = {
      Unit = {
        Description = "Agent voice hotkeys and selected session";
        # Deepgram needs DEEPGRAM_API_KEY from the sops-nix rendered file, read
        # once at exec. Order after decryption so the backend is registered
        # and offered as an option instead of silently dropped at boot.
        After = [ "sops-nix.service" ];
        Wants = [ "sops-nix.service" ];
      };
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
    home.activation.piVoiceDirectories = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
      ${pkgs.systemd}/bin/systemctl --user stop \
        codex-voice.service codex-voice-stt.service codex-voice-tts.service \
        pi-voice-api.service pi-voice-stt.service pi-voice-tts.service \
        personaplex-open.service personaplex.service >/dev/null 2>&1 || true
      oldData="$HOME/.local/share/codex-voice"
      if [ -e "$oldData" ] && [ ! -e ${lib.escapeShellArg dataDir} ]; then
        ${pkgs.coreutils}/bin/mv "$oldData" ${lib.escapeShellArg dataDir}
      fi
      install -d -m 0700 ${lib.escapeShellArg dataDir}
    '';
  };
}
