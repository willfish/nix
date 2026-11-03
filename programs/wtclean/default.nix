{ pkgs }:
pkgs.writeShellApplication {
  name = "wtclean";
  runtimeInputs = [
    pkgs.git
    pkgs.gh
    pkgs.jq
    pkgs.coreutils
    pkgs.gnugrep
    pkgs.gnused
  ];
  text = builtins.readFile ./wtclean.sh;
}
