{ pkgs, hostName }:
pkgs.stdenv.isLinux
&& builtins.elem hostName [
  "andromeda"
  "foundation"
]
