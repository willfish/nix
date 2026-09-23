{
  pkgs,
  lib,
  hostName,
  ...
}:
{
  _module.args.hostName = lib.mkDefault null;
  _module.args.isGraphicalLinux = pkgs.stdenv.isLinux && hostName != "terminus";

  _module.args.readSopsSecret = pkgs.writeShellScriptBin "read-sops-secret" ''
    set -euo pipefail

    if [ "$#" -ne 1 ]; then
      echo "usage: read-sops-secret <secret-file>" >&2
      exit 64
    fi

    secret_file="$1"

    if [ ! -r "$secret_file" ]; then
      echo "secret file is not readable: $secret_file" >&2
      exit 66
    fi

    value="$(<"$secret_file")"
    value="''${value%\"}"
    value="''${value#\"}"
    printf '%s' "$value"
  '';

  imports = [
    ./capabilities.nix
    ./appearance.nix
    ./hyprland.nix
    ./config.nix
    ./desktop-files.nix
    ./voice.nix
    ./darwin.nix
    ./email.nix
    ./environment.nix
    ./git.nix
    ./llm-harness.nix
    ./local-llm.nix
    ./hermes.nix
    ./mcp.nix
    ./neovim.nix
    ./network.nix
    ./packages.nix
    ./pi.nix
    ./programs.nix
    ./prompt-capture-file.nix
    ./shells.nix
    ./stylix.nix
  ];
}
