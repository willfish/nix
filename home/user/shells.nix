{ pkgs, lib, ... }:
let
  inherit (pkgs) stdenv;
  aliases = {
    a = "tmux attach";
    ag = "rg";
    la = "lsd -la";
    ll = "lsd -l";
    psql = "pgcli";
    tm = "tmux";
    v = "nvim";
    vi = "nvim";
    vim = "nvim";
    vimdiff = "nvim -d";
  };
  abbreviations = {
    a = "tmux attach";
    ag = "rg";

    cdn = "cd ~/Notes";
    cdr = "cd ~/Repositories";
    hm = "cd ~/Repositories/hmrc";

    rc = "bundle exec rails console";
    rr = "bundle exec rails routes --expanded";
    rs = "bundle exec rails server";
    sk = "bundle exec sidekiq";
    t = "bundle exec rspec --format p";

    tg = "terragrunt";

    tm = "tmux";

    prod = "cd ~/Repositories/hmrc/trade-tariff-platform-aws-terraform/environments/production";
    stag = "cd ~/Repositories/hmrc/trade-tariff-platform-aws-terraform/environments/staging";
    dev = "cd ~/Repositories/hmrc/trade-tariff-platform-aws-terraform/environments/development";
  }
  // lib.optionalAttrs stdenv.isLinux {
    pbcopy = "xclip -selection clipboard";
    pbpaste = "xclip -selection clipboard -o";
  };
in
{
  programs.bash = {
    enable = true;
    shellAliases = aliases;
  };

  programs.fish = {
    package = pkgs.fish;
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;

    interactiveShellInit =
      (
        if stdenv.isDarwin then
          ''
            # Source Nix daemon environment for fish on macOS (sets PATH, NIX_SSL_CERT_FILE, etc.)
            if test -e /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.fish
              source /nix/var/nix/profiles/default/etc/profile.d/nix-daemon.fish
            end
          ''
        else
          ""
      )
      + ''
        set -gx AWS_DEFAULT_REGION eu-west-2
        set -gx AWS_REGION eu-west-2
        set -gx ERL_AFLAGS "-kernel shell_history enabled"
        set -gx SAM_CLI_TELEMETRY 0
      '';

    functions = {
      __git_worktree_path_for_branch = ''
        set -l target $argv[1]

        if test -z "$target"
          return 1
        end

        command git worktree list --porcelain | awk -v branch="refs/heads/$target" '
          /^worktree / { worktree = substr($0, 10) }
          /^branch / && $2 == branch { print worktree; exit }
        '
      '';
      git = ''
        if test (count $argv) -eq 2
          set -l subcommand $argv[1]
          set -l target $argv[2]

          if contains -- $subcommand switch checkout
            if command git rev-parse --is-inside-work-tree >/dev/null 2>/dev/null
              if command git show-ref --verify --quiet "refs/heads/$target"
                set -l current_root (command git rev-parse --show-toplevel 2>/dev/null)
                set -l target_worktree (__git_worktree_path_for_branch $target)

                if test -n "$target_worktree"; and test "$target_worktree" != "$current_root"
                  cd "$target_worktree"
                  return $status
                end
              end
            end
          end
        end

        command git $argv
      '';
      gitignore = "curl -sL https://www.gitignore.io/api/$argv";
      today = ''notes_on (date +"%Y-%m-%d") today.md'';
      yesterday = ''notes_on (date -d yesterday +"%Y-%m-%d") today.md'';
      tomorrow = ''notes_on (date +%F -d "tomorrow") today.md'';
      logs = ''echo dev_hub_log\nidentity_log\nfrontend_log\nbackend_log\nadmin_log\nduty_log\nsearch_query_log | fzf | xargs -- echo'';
      frontend_log = ''log_for "https://www.trade-tariff.service.gov.uk/healthcheck" trade-tariff-frontend'';
      backend_log = ''log_for "https://www.trade-tariff.service.gov.uk/api/v2/healthcheck" trade-tariff-backend'';
      admin_log = ''log_for "https://admin.trade-tariff.service.gov.uk/healthcheck" trade-tariff-admin'';
      dev_hub_log = ''log_for "https://hub.trade-tariff.service.gov.uk/healthcheck" trade-tariff-dev-hub'';
      identity_log = ''log_for "https://id.trade-tariff.service.gov.uk/healthcheck" identity'';
      all_logs = ''
        frontend_log
        backend_log
        admin_log
        dev_hub_log
        identity_log
      '';
    };
  };
  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.package = pkgs.zoxide;
}
