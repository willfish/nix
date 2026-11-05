{ pkgs, hostName }:
let
  linux = pkgs.stdenv.isLinux;
in
rec {
  # Hex dictation and its desktop shortcuts.
  stt =
    linux
    && builtins.elem hostName [
      "andromeda"
      "foundation"
    ];
  # Temporarily retire the Pi voice bridge and menu while using Hex.
  pi = stt && false;
}
