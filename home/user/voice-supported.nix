{ pkgs, hostName }:
let
  linux = pkgs.stdenv.isLinux;
in
{
  # Whisper dictation on Vulkan. Andromeda uses NVIDIA; Foundation uses Radeon.
  stt =
    linux
    && builtins.elem hostName [
      "andromeda"
      "foundation"
    ];
  # Qwen3 TTS needs Andromeda's CUDA stack.
  tts = linux && hostName == "andromeda";
}
