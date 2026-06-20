{
  config,
  lib,
  pkgs,
  readSopsSecret,
  ...
}:
let
  braveCdpEndpoint = "http://127.0.0.1:9222";
  inherit (import ./llm-mcps.nix { inherit config; }) servers;
  enabled = name: builtins.any (server: server.name == name) servers;
  agentBrowserVersion = "0.36.0";
  agentBrowserRelease =
    {
      x86_64-linux = {
        asset = "agent-browser-linux-musl-x64";
        hash = "sha256-ogzCpSAqSPWCA3KAPe281fVW3/eolCHxsPJhKWKxBxg=";
      };
      aarch64-linux = {
        asset = "agent-browser-linux-musl-arm64";
        hash = "sha256-HKfgA8nLGF8XT8geUaYJ2yfHfjv+AKDt/2Boj4zRT4g=";
      };
      x86_64-darwin = {
        asset = "agent-browser-darwin-x64";
        hash = "sha256-RdmsBhp9cuYer/kFMm4uGTZfTa2xIULqLy122EaJxwg=";
      };
      aarch64-darwin = {
        asset = "agent-browser-darwin-arm64";
        hash = "sha256-shBqs52wg457F3L38m92BRjeVtCQUxUMVvnd3xWvmX0=";
      };
    }
    .${pkgs.stdenv.hostPlatform.system}
      or (throw "agent-browser has no release binary for ${pkgs.stdenv.hostPlatform.system}");
  agentBrowser = pkgs.stdenvNoCC.mkDerivation {
    pname = "agent-browser";
    version = agentBrowserVersion;
    src = pkgs.fetchurl {
      url = "https://github.com/vercel-labs/agent-browser/releases/download/v${agentBrowserVersion}/${agentBrowserRelease.asset}";
      inherit (agentBrowserRelease) hash;
    };
    dontUnpack = true;
    installPhase = ''
      runHook preInstall
      install -Dm755 "$src" "$out/bin/agent-browser"
      runHook postInstall
    '';
    meta = {
      description = "Rust-native browser automation CLI and MCP server";
      homepage = "https://github.com/vercel-labs/agent-browser";
      license = lib.licenses.asl20;
      mainProgram = "agent-browser";
      platforms = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
    };
  };
  slackMcpServer = pkgs.callPackage ./mcp-packages/slack-mcp-server.nix { };
  telegramMcpServer = pkgs.callPackage ./mcp-packages/telegram-mcp.nix { };
  # One-time Telethon login for the file-based session used by mcp-telegram.
  telegramLoginScript = pkgs.writeTextFile {
    name = "telegram-mcp-login.py";
    destination = "/telegram-mcp-login.py";
    executable = true;
    text = ''
      #!${telegramMcpServer.passthru.interpreter}/bin/python3
      import os

      from telethon.sync import TelegramClient

      client = TelegramClient(
          os.environ["TELEGRAM_SESSION_NAME"],
          int(os.environ["TELEGRAM_API_ID"]),
          os.environ["TELEGRAM_API_HASH"],
      )
      # start() connects and checks authorization, prompting only if needed.
      try:
          client.start()
          me = client.get_me()
          print(f"Logged in as {me.first_name} (@{me.username})")
      finally:
          client.disconnect()
    '';
  };

in
{
  home.packages = [
    agentBrowser
  ]
  ++
    map
      (
        server:
        {
          jira = pkgs.uv;
          github = pkgs.github-mcp-server;
          browser = agentBrowser;
          browser-playwright = pkgs.playwright-mcp;
          terraform = pkgs.terraform-mcp-server;
          nixos = pkgs.mcp-nixos;
          slack = slackMcpServer;
          telegram = telegramMcpServer;
        }
        .${server.name}
      )
      (
        builtins.filter (
          server:
          !(server.private or false)
          && !(builtins.elem server.name [
            "jira"
            "browser"
          ])
        ) servers
      );

  home.file.".local/bin/mcp-github" = lib.mkIf (enabled "github") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      GITHUB_PERSONAL_ACCESS_TOKEN="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.GITHUB_CLASSIC_PERSONAL_ACCESS_TOKEN.path})"
      : "''${GITHUB_PERSONAL_ACCESS_TOKEN:?GITHUB_CLASSIC_PERSONAL_ACCESS_TOKEN must be set in sops-nix secrets}"
      export GITHUB_PERSONAL_ACCESS_TOKEN

      exec ${pkgs.github-mcp-server}/bin/github-mcp-server \
        --toolsets=default,actions,notifications,code_security,secret_protection,dependabot \
        stdio
    '';
  };

  home.file.".local/bin/mcp-brave" = lib.mkIf (enabled "browser-playwright") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      if ! ${pkgs.curl}/bin/curl --fail --silent --show-error --max-time 2 \
        ${lib.escapeShellArg "${braveCdpEndpoint}/json/version"} >/dev/null; then
        echo "Visible Brave CDP endpoint is unavailable at ${braveCdpEndpoint}; refusing to launch another browser" >&2
        exit 1
      fi

      exec ${pkgs.playwright-mcp}/bin/playwright-mcp \
        --cdp-endpoint ${lib.escapeShellArg braveCdpEndpoint} \
        --browser chrome \
        --caps vision,pdf,devtools
    '';
  };

  home.file.".local/bin/mcp-agent-browser" = lib.mkIf (enabled "browser") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      if ! ${pkgs.curl}/bin/curl --fail --silent --show-error --max-time 2 \
        ${lib.escapeShellArg "${braveCdpEndpoint}/json/version"} >/dev/null; then
        echo "Visible Brave CDP endpoint is unavailable at ${braveCdpEndpoint}; refusing to launch another browser" >&2
        exit 1
      fi

      export AGENT_BROWSER_CDP=${lib.escapeShellArg braveCdpEndpoint}
      export AGENT_BROWSER_SESSION="''${AGENT_BROWSER_SESSION:-mcp-visible-brave}"
      export AGENT_BROWSER_PIN_TAB=1
      export AGENT_BROWSER_CONTENT_BOUNDARIES=1
      export AGENT_BROWSER_MAX_OUTPUT=50000
      export AGENT_BROWSER_NO_AUTO_DIALOG=1

      exec ${agentBrowser}/bin/agent-browser mcp --tools core
    '';
  };

  home.file.".local/bin/mcp-terraform" = lib.mkIf (enabled "terraform") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      exec ${pkgs.terraform-mcp-server}/bin/terraform-mcp-server stdio
    '';
  };

  home.file.".local/bin/mcp-nixos" = lib.mkIf (enabled "nixos") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      exec ${pkgs.mcp-nixos}/bin/mcp-nixos
    '';
  };

  home.file.".local/bin/mcp-slack" = lib.mkIf (enabled "slack") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      # Canonical Slack auth: browser session tokens in sops
      # (SLACK_XOXC + SLACK_COOKIE_D). Optional local refresh file can
      # override when tokens have just been re-extracted via CDP.
      session_env="''${XDG_CONFIG_HOME:-$HOME/.config}/slack-session/tokens.env"
      if [ -r "$session_env" ]; then
        # Read legacy raw assignments as data, never evaluate shell text.
        while IFS='=' read -r key value || [ -n "$key" ]; do
          case "$key" in
            SLACK_XOXC)
              if [[ "$value" =~ ^xoxc-[A-Za-z0-9%._-]+$ ]]; then SLACK_XOXC="$value"; fi
              ;;
            SLACK_COOKIE_D)
              if [[ "$value" =~ ^xoxd-[A-Za-z0-9%._/+=-]+$ ]]; then SLACK_COOKIE_D="$value"; fi
              ;;
            SLACK_TEAM_ID)
              if [[ "$value" =~ ^[A-Za-z0-9_-]+$ ]]; then SLACK_TEAM_ID="$value"; fi
              ;;
          esac
        done < "$session_env"
        :
      fi

      if [ -z "''${SLACK_XOXC:-}" ]; then
        SLACK_XOXC="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.SLACK_XOXC.path})"
      fi
      if [ -z "''${SLACK_COOKIE_D:-}" ]; then
        SLACK_COOKIE_D="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.SLACK_COOKIE_D.path})"
      fi
      if [ -z "''${SLACK_TEAM_ID:-}" ]; then
        SLACK_TEAM_ID="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.SLACK_TEAM_ID.path})"
      fi

      : "''${SLACK_XOXC:?SLACK_XOXC must be set (sops or slack-session/tokens.env)}"
      : "''${SLACK_COOKIE_D:?SLACK_COOKIE_D must be set (sops or slack-session/tokens.env)}"
      : "''${SLACK_TEAM_ID:?SLACK_TEAM_ID must be set}"

      # The native server prioritizes OAuth tokens over browser credentials.
      unset SLACK_MCP_XOXP_TOKEN SLACK_MCP_XOXB_TOKEN
      export SLACK_MCP_XOXC_TOKEN="$SLACK_XOXC"
      export SLACK_MCP_XOXD_TOKEN="$SLACK_COOKIE_D"
      export SLACK_MCP_ADD_MESSAGE_TOOL=true
      export SLACK_MCP_REACTION_TOOL=true

      # Preserve existing read/write tools and add search. Credentials stay in
      # the environment; the native server manages its own workspace caches.
      exec ${slackMcpServer}/bin/slack-mcp-server \
        --transport stdio \
        --enabled-tools conversations_history,conversations_replies,conversations_add_message,reactions_add,conversations_search_messages,channels_list,users_search
    '';
  };

  home.file.".local/bin/slack-refresh-session" = lib.mkIf (enabled "slack") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail
      export OUT_FILE="''${XDG_CONFIG_HOME:-$HOME/.config}/slack-session/tokens.env"
      export SLACK_CDP_URL="''${SLACK_CDP_URL:-http://127.0.0.1:9222}"
      export SLACK_REFRESH_PYTHON="${pkgs.python3.withPackages (ps: [ ps.websockets ])}/bin/python3"
      exec ${pkgs.bash}/bin/bash ${../config}/llm/scripts/slack-refresh-session.sh
    '';
  };

  home.file.".local/bin/mcp-telegram" = lib.mkIf (enabled "telegram") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      TELEGRAM_API_ID="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.TELEGRAM_API_ID.path})"
      TELEGRAM_API_HASH="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.TELEGRAM_API_HASH.path})"

      : "''${TELEGRAM_API_ID:?TELEGRAM_API_ID must be set in sops-nix secrets}"
      : "''${TELEGRAM_API_HASH:?TELEGRAM_API_HASH must be set in sops-nix secrets}"
      export TELEGRAM_API_ID TELEGRAM_API_HASH

      # File-based Telethon session (the session file is full account
      # access): private local state, never in the repo or the Nix store.
      state_root="''${XDG_STATE_HOME:-$HOME/.local/state}"
      state_dir="$state_root/telegram-mcp"
      install -d -m 0700 "$state_dir"
      export XDG_STATE_HOME="$state_root"
      export TELEGRAM_SESSION_NAME="$state_dir/session"
      # Upstream defaults this to CWD-relative data/transcripts; pin it.
      export TELEGRAM_TRANSCRIPT_CACHE_DIR="$state_dir/transcripts"
      export TELEGRAM_LOG_FILE="$state_dir/mcp_errors.log"

      # Use these file roots when the client cannot supply usable MCP Roots.
      export TELEGRAM_ALLOW_SERVER_ROOTS_FALLBACK=true
      exec ${telegramMcpServer}/bin/telegram-mcp "$state_dir" /tmp "$HOME"
    '';
  };

  home.file.".local/bin/telegram-mcp-login" = lib.mkIf (enabled "telegram") {
    executable = true;
    text = ''
      #!${pkgs.bash}/bin/bash
      set -euo pipefail

      TELEGRAM_API_ID="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.TELEGRAM_API_ID.path})"
      TELEGRAM_API_HASH="$(${readSopsSecret}/bin/read-sops-secret ${lib.escapeShellArg config.sops.secrets.TELEGRAM_API_HASH.path})"

      : "''${TELEGRAM_API_ID:?TELEGRAM_API_ID must be set in sops-nix secrets}"
      : "''${TELEGRAM_API_HASH:?TELEGRAM_API_HASH must be set in sops-nix secrets}"
      export TELEGRAM_API_ID TELEGRAM_API_HASH

      state_root="''${XDG_STATE_HOME:-$HOME/.local/state}"
      state_dir="$state_root/telegram-mcp"
      install -d -m 0700 "$state_dir"
      export XDG_STATE_HOME="$state_root"
      export TELEGRAM_SESSION_NAME="$state_dir/session"

      exec "${telegramLoginScript}/telegram-mcp-login.py"
    '';
  };

  home.activation.configureGithubCliAuth = lib.hm.dag.entryAfter [ "sops-nix" ] ''
    github_token_file=${lib.escapeShellArg config.sops.secrets.GITHUB_CLASSIC_PERSONAL_ACCESS_TOKEN.path}
    if [ -r "$github_token_file" ]; then
      github_token="$(${readSopsSecret}/bin/read-sops-secret "$github_token_file")"
      if ! printf '%s' "$github_token" | ${pkgs.gh}/bin/gh auth login --hostname github.com --with-token >/dev/null; then
        echo "warning: failed to configure GitHub CLI auth from sops secret" >&2
      fi
    fi
  '';
}
