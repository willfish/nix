{ pkgs, ... }:

# Expose the capture helper on PATH so `prompt-capture stop <tool>` and
# `prompt-capture logs <tool>` are usable without a store path.
let
  promptCapture = import ./prompt-capture.nix { inherit pkgs; };
in
{
  home.file.".local/bin/prompt-capture" = {
    source = "${promptCapture}/bin/prompt-capture";
  };
}
