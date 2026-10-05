{ pkgs, hostName }:
let
  linux = pkgs.stdenv.isLinux;
in
{
  # Voice controller, hotkeys, pill and cloud speech.
  stt =
    linux
    && builtins.elem hostName [
      "andromeda"
      "foundation"
    ];
  # Local Whisper and the loopback API that starts it. Laptops stay cloud-only.
  localStt = linux && hostName == "andromeda";
  # Qwen3 TTS needs Andromeda's CUDA stack.
  tts = linux && hostName == "andromeda";
}
