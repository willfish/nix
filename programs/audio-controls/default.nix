{ pkgs }:
pkgs.writeShellApplication {
  name = "hypr-audio-controls";
  runtimeInputs = with pkgs; [
    pipewire
    wireplumber
    jq
  ];
  text = builtins.readFile ./audio-controls.sh;
}
