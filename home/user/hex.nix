{
  config,
  pkgs,
  lib,
  hostName,
  ...
}:
let
  voice = import ./voice-supported.nix { inherit pkgs hostName; };
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
    };
  };
  # Leave Andromeda's VRAM for local models without disabling Hex's UI renderer.
  systemd.user.services.hex = lib.mkIf (voice.stt && hostName == "andromeda") {
    Service.Environment = [ "TRANSCRIBE_BACKENDS=cpu" ];
  };
  # The running portal may retain the application dirs from before a profile
  # switch. Publish its app ID in the stable user directory as well.
  xdg.dataFile."applications/anomaly.hex.desktop" = lib.mkIf voice.stt {
    source = "${config.programs.hex.package}/share/applications/anomaly.hex.desktop";
  };
}
