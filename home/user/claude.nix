{ pkgs, ... }:
{
  home.file.".local/bin/claude" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      if [ -z "''${CLAUDE_CODE_OAUTH_TOKEN:-}" ]; then
        claude_token_file="$HOME/.config/sops-nix/secrets/CLAUDE_CODE_OAUTH_TOKEN"
        if [ -r "$claude_token_file" ]; then
          CLAUDE_CODE_OAUTH_TOKEN="$(<"$claude_token_file")"
          CLAUDE_CODE_OAUTH_TOKEN="''${CLAUDE_CODE_OAUTH_TOKEN%\"}"
          CLAUDE_CODE_OAUTH_TOKEN="''${CLAUDE_CODE_OAUTH_TOKEN#\"}"
          export CLAUDE_CODE_OAUTH_TOKEN
        fi
      fi

      exec ${pkgs.claude-code}/bin/claude "$@"
    '';
  };
}
