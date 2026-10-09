{ pkgs, hostName }:
let
  linux = pkgs.stdenv.isLinux;
in
{
  # Voice controller, hotkeys, status card and Deepgram speech.
  stt =
    linux
    && builtins.elem hostName [
      "andromeda"
      "foundation"
    ];
}
