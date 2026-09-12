{ pkgs, hostName }:
# Voice needs Andromeda's NVIDIA/CUDA stack. Foundation is AMD and stays off.
pkgs.stdenv.isLinux && hostName == "andromeda"
