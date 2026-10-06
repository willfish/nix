{ pkgs, ... }:
let
  adapter = import ./prompt-capture-package.nix { inherit pkgs; };
in
# Opt-in prompt capture for the LLM CLIs (CAPTURE_PROMPTS=1).
#
# The script lives in prompt-capture.sh (shfmt-formatted, shellchecked);
# it cannot reference pkgs, so the shebang and the mitmdump closure path
# are prepended here and the result is installed as /bin/prompt-capture.
pkgs.writeTextFile {
  name = "prompt-capture";
  executable = true;
  destination = "/bin/prompt-capture";
  # "target" is a shell variable set by the builder before this is eval'd,
  # so it must be double-quoted to expand at build time.
  checkPhase = "${pkgs.bash}/bin/bash -n -O extglob \"$target\"";
  text = ''
    #!${pkgs.bash}/bin/bash
    MITMDUMP="${adapter}/bin/prompt-capture-mitm"
    FLOCK="${pkgs.flock}/bin/flock"
    ${builtins.readFile ./prompt-capture.sh}
  '';
}
