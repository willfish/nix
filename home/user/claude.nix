{ pkgs, ... }:
let
  promptCapture = import ./prompt-capture.nix { inherit pkgs; };
in
{
  home.file.".local/bin/claude" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      if [ -n "''${CAPTURE_PROMPTS:-}" ] && [ "''${CAPTURE_PROMPTS:-}" != "0" ]; then
        exec ${promptCapture}/bin/prompt-capture claude -- ${pkgs.claude-code}/bin/claude "$@"
      fi

      exec ${pkgs.claude-code}/bin/claude "$@"
    '';
  };
}
