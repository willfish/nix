{
  config,
  pkgs,
  readSopsSecret,
  ...
}:
{
  home.file.".local/bin/claude" = {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      if [ -z "''${CLAUDE_CODE_OAUTH_TOKEN:-}" ]; then
        claude_token_file="${config.sops.secrets.CLAUDE_CODE_OAUTH_TOKEN.path}"
        if [ -r "$claude_token_file" ]; then
          CLAUDE_CODE_OAUTH_TOKEN="$(${readSopsSecret}/bin/read-sops-secret "$claude_token_file")"
          export CLAUDE_CODE_OAUTH_TOKEN
        fi
      fi

      exec ${pkgs.claude-code}/bin/claude "$@"
    '';
  };
}
