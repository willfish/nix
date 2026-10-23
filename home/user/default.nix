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

    if [ "$#" -gt 1 ]; then
      echo "usage: read-sops-secret [secret-file | SECRET_NAME]" >&2
      exit 64
    fi

    secrets_dir="''${SOPS_NIX_SECRETS_DIR:-''${XDG_CONFIG_HOME:-$HOME/.config}/sops-nix/secrets}"

    if [ "$#" -eq 0 ]; then
      names=()
      for candidate in "$secrets_dir"/*; do
        [ -f "$candidate" ] && [ -r "$candidate" ] || continue
        names+=("''${candidate##*/}")
      done
      if [ "''${#names[@]}" -eq 0 ]; then
        echo "read-sops-secret: no readable secrets in $secrets_dir" >&2
        exit 66
      fi
      name="$(printf '%s\n' "''${names[@]}" | ${pkgs.fzf}/bin/fzf --prompt='secret> ')" || exit $?
      [ -n "$name" ] || exit 130
      secret_file="$secrets_dir/$name"
    elif [ -e "$1" ] || [[ "$1" == */* ]]; then
      secret_file="$1"
    else
      secret_file="$secrets_dir/$1"
    fi

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
    ./daily-workflow.nix
    ./email.nix
    ./environment.nix
    ./git.nix
    ./llm-harness.nix
    ./local-llm.nix
    ./strata-relay.nix
    ./hermes.nix
    ./mcp.nix
    ./arxiv-library.nix
    ./neovim.nix
    ./network.nix
    ./packages.nix
    ./pi.nix
    ./opencode.nix
    ./programs.nix
    ./prompt-capture-file.nix
    ./shells.nix
    ./stylix.nix
  ];
}
