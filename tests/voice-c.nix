{ pkgs }:
# Test the same native executables and library that Home Manager installs.
import ../home/user/voice-c-package.nix { inherit pkgs; }
