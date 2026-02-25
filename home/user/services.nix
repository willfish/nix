{ pkgs, lib, ... }:
let
  inherit (pkgs) stdenv;

  repos = [
    "trade-tariff-backend"
    "trade-tariff-frontend"
    "trade-tariff-admin"
    "identity"
    "trade-tariff-dev-hub"
    "routing-filter"
    "uktt"
    "trade-tariff-classification-examples"
    "trade-tariff-tech-docs"
  ];

  repoDir = "$HOME/Repositories/hmrc";

  repoList = lib.concatMapStringsSep "\n" (r: "  \"${r}\"") repos;

  dependabot-triage = pkgs.writeShellScriptBin "claude-dependabot-triage" ''
    REPOS=(
    ${repoList}
    )

    for repo in "''${REPOS[@]}"; do
      dir="${repoDir}/$repo"

      if [ ! -d "$dir" ]; then
        echo "Skipping $repo: directory not found"
        continue
      fi

      echo "Processing $repo..."

      cd "$dir"
      git checkout "$(git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null | sed 's@^refs/remotes/origin/@@' || echo main)"
      git pull

      claude -p "You are triaging Dependabot PRs for the $repo repository.

    1. List open Dependabot PRs: gh pr list --author 'app/dependabot' --state open --json number,title,url
    2. For each PR:
       - Determine if it is a patch, minor, or major update from the title
       - For PATCH updates: approve and merge with: gh pr review <number> --approve && gh pr merge <number> --admin --squash
       - For MINOR and MAJOR updates:
         a. Read the release notes/changelog linked in the PR body
         b. Check for breaking changes
         c. Add a needs-review label: gh pr edit <number> --add-label needs-review
         d. Post a summary to Slack #dev-updates using: curl -X POST -H 'Authorization: Bearer '\$SLACK_BOT_TOKEN -H 'Content-type: application/json' --data '{\"channel\":\"#dev-updates\",\"text\":\"Dependabot PR needs review in $repo: <title> (<url>)\"}' https://slack.com/api/chat.postMessage
    3. If there are no open Dependabot PRs, skip this repo silently." \
        --dangerously-skip-permissions
    done
  '';

  ruby-upgrade = pkgs.writeShellScriptBin "claude-ruby-upgrade" ''
    REPOS=(
    ${repoList}
    )

    GUIDE="$HOME/.claude/guides/ruby-upgrades.md"

    for repo in "''${REPOS[@]}"; do
      dir="${repoDir}/$repo"

      if [ ! -d "$dir" ]; then
        echo "Skipping $repo: directory not found"
        continue
      fi

      echo "Processing $repo..."

      cd "$dir"
      git checkout "$(git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null | sed 's@^refs/remotes/origin/@@' || echo main)"
      git pull

      claude -p "You are upgrading Ruby for the $repo repository. Follow the guide at $GUIDE exactly.

    Check if a newer stable Ruby version is available compared to the current .ruby-version.
    If an upgrade is available, follow the upgrade process in the guide.
    If this repo is trade-tariff-backend, also update .tool-versions and .devcontainer/Dockerfile.
    If already on the latest version or an upgrade PR already exists, skip this repo." \
        --dangerously-skip-permissions
    done
  '';

  pathDeps = lib.makeBinPath (
    with pkgs;
    [
      claude-code
      gh
      git
      ruby
      bundler
      curl
      coreutils
      bash
    ]
  );
in
lib.mkIf stdenv.isLinux {
  systemd.user.services = {
    claude-dependabot-triage = {
      Unit = {
        Description = "Claude Code Dependabot PR triage";
      };
      Service = {
        Type = "oneshot";
        ExecStart = "${dependabot-triage}/bin/claude-dependabot-triage";
        Environment = [
          "PATH=${pathDeps}"
          "HOME=%h"
        ];
      };
    };

    claude-ruby-upgrade = {
      Unit = {
        Description = "Claude Code Ruby version upgrade";
      };
      Service = {
        Type = "oneshot";
        ExecStart = "${ruby-upgrade}/bin/claude-ruby-upgrade";
        Environment = [
          "PATH=${pathDeps}"
          "HOME=%h"
        ];
      };
    };
  };

  systemd.user.timers = {
    claude-dependabot-triage = {
      Unit = {
        Description = "Daily Dependabot PR triage";
      };
      Timer = {
        OnCalendar = "*-*-* 08:00:00";
        Persistent = true;
      };
      Install = {
        WantedBy = [ "timers.target" ];
      };
    };

    claude-ruby-upgrade = {
      Unit = {
        Description = "Weekly Ruby version upgrade check";
      };
      Timer = {
        OnCalendar = "Mon *-*-* 09:00:00";
        Persistent = true;
      };
      Install = {
        WantedBy = [ "timers.target" ];
      };
    };
  };
}
