{
  config,
  pkgs,
  lib,
  ...
}:
let
  inherit (pkgs) stdenv;
  aliases = {
    a = "herdr";
    ag = "rg";
    la = "lsd -la";
    ll = "lsd -l";
    tm = "tmux";
    v = "nvim";
    vi = "nvim";
    vim = "nvim";
    vimdiff = "nvim -d";
    yolo = "agy --dangerously-skip-permissions";
  }
  // lib.optionalAttrs config.dotfiles.capabilities.work { psql = "pgcli"; };
  abbreviations = {
    a = "herdr";
    ag = "rg";

    cdn = "cd ~/Notes";
    cdr = "cd ~/Repositories";
    tm = "tmux";
  }
  // lib.optionalAttrs config.dotfiles.capabilities.development {
    bundle = "bundle install";
    rc = "bundle exec rails console";
    rr = "bundle exec rails routes --expanded";
    rs = "bundle exec rails server";
    sk = "bundle exec sidekiq";
    t = "bundle exec rspec --format p";

  }
  // lib.optionalAttrs config.dotfiles.capabilities.work {
    tg = "terragrunt";
  }
  // lib.optionalAttrs (stdenv.isLinux && config.dotfiles.capabilities.desktop) {
    pbcopy = "xclip -selection clipboard";
    pbpaste = "xclip -selection clipboard -o";
    aw = "awake toggle";
  };
  git-cleanup = pkgs.writeShellScriptBin "git-cleanup" ''
    set -euo pipefail

    git fetch --prune origin
    default_ref="$(git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null || true)"
    if [[ "$default_ref" != refs/remotes/origin/* ]] || ! git show-ref --verify --quiet "$default_ref"; then
      echo "git-cleanup: origin default branch is unknown or missing; no cleanup performed" >&2
      exit 1
    fi
    default="''${default_ref#refs/remotes/origin/}"
    current_root="$(git rev-parse --show-toplevel)"
    git pull --ff-only

    # Prune administrative records only. Automatic housekeeping must not delete
    # service state belonging to an unavailable or unpublished worktree.
    ${pkgs.git}/bin/git worktree prune

    eligible_branch() {
      local branch="$1" upstream
      [ "$branch" != "$default" ] || return 1
      upstream="$(git for-each-ref --format='%(upstream)' "refs/heads/$branch")" || return 1
      case "$upstream" in
        refs/remotes/origin/*) ;;
        *) return 1 ;;
      esac
      if git show-ref --verify --quiet "$upstream"; then
        return 1
      fi
      git merge-base --is-ancestor "refs/heads/$branch" "$default_ref"
    }

    remove_if_safe() {
      local wt="$1" branch="$2" state
      [ "$wt" != "$current_root" ] && [ -d "$wt" ] || return 0
      eligible_branch "$branch" || return 0
      state="$(git -C "$wt" status --porcelain --untracked-files=all --ignored)" || return 1
      [ -z "$state" ] || return 0

      if git worktree remove "$wt"; then
        echo "Removed worktree: $wt ($branch)"
        git branch -d "$branch" || echo "Branch $branch retained by Git"
      else
        echo "Could not remove worktree $wt; retained remaining state" >&2
        return 1
      fi
    }

    wt="" branch="" locked=0
    git worktree list --porcelain -z | while IFS= read -r -d "" field; do
      case "$field" in
        worktree\ *) wt="''${field#worktree }" ;;
        branch\ refs/heads/*) branch="''${field#branch refs/heads/}" ;;
        locked|locked\ *) locked=1 ;;
        "")
          if [ "$locked" -eq 0 ] && [ -n "$branch" ]; then
            remove_if_safe "$wt" "$branch"
          fi
          wt="" branch="" locked=0
          ;;
      esac
    done

    # Apply the same policy to branches without a worktree. Git additionally
    # refuses deletion of branches still checked out in retained worktrees.
    git for-each-ref --format='%(refname:short)' --merged="$default_ref" refs/heads |
      while IFS= read -r branch; do
        eligible_branch "$branch" || continue
        git branch -d "$branch" 2>/dev/null || true
      done
  '';
  git-cm = pkgs.writeShellScriptBin "git-cm" ''
    set -euo pipefail

    if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --default-branch ]; }; then
      echo "usage: git cm [--default-branch]" >&2
      exit 2
    fi

    default_ref="$(git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null || true)"
    if [[ "$default_ref" != refs/remotes/origin/* ]] || ! git show-ref --verify --quiet "$default_ref"; then
      # Recreated repositories may lack origin/HEAD. Ask origin rather than
      # guessing main/master, and keep resolver stdout usable by Fish.
      git fetch origin >&2
      git remote set-head origin --auto >&2
      default_ref="$(git symbolic-ref refs/remotes/origin/HEAD)"
    fi
    if [[ "$default_ref" != refs/remotes/origin/* ]] || ! git show-ref --verify --quiet "$default_ref"; then
      echo "git-cm: origin default branch is unknown or missing; no checkout performed" >&2
      exit 1
    fi
    default="''${default_ref#refs/remotes/origin/}"
    if [ "''${1:-}" = --default-branch ]; then
      printf '%s\n' "$default"
      exit 0
    fi

    git switch "$default"
    git-cleanup
  '';
  awake = pkgs.writeShellScriptBin "awake" ''
    #!${pkgs.bash}/bin/bash
    set -euo pipefail

    WHO=awake
    WHY="Stay awake"
    WHAT=idle:sleep:handle-lid-switch
    PID_FILE="''${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/awake.pid"

    is_running() {
      [[ -f "$PID_FILE" ]] || return 1
      kill -0 "$(<"$PID_FILE")" 2>/dev/null
    }

    usage() {
      echo "usage: awake {start|stop|status|toggle}" >&2
      echo "  Blocks idle, suspend, and lid-close sleep via systemd-inhibit." >&2
    }

    start() {
      if is_running; then
        echo "already awake (pid $(<"$PID_FILE"))"
        return 0
      fi

      mkdir -p "$(dirname "$PID_FILE")"
      ${pkgs.systemd}/bin/systemd-inhibit \
        --what="$WHAT" \
        --who="$WHO" \
        --why="$WHY" \
        --mode=block \
        sleep infinity &
      echo $! > "$PID_FILE"

      sleep 0.2
      if ! is_running; then
        rm -f "$PID_FILE"
        echo "failed to start awake inhibitor" >&2
        return 1
      fi

      echo "awake (pid $(<"$PID_FILE"))"
    }

    stop() {
      if ! is_running; then
        rm -f "$PID_FILE"
        echo "not awake"
        return 0
      fi

      kill "$(<"$PID_FILE")" 2>/dev/null || true
      rm -f "$PID_FILE"
      echo "stopped awake"
    }

    status() {
      if is_running; then
        echo "awake (pid $(<"$PID_FILE"))"
        ${pkgs.systemd}/bin/systemd-inhibit --list 2>/dev/null \
          | ${pkgs.gnugrep}/bin/grep -E "^WHO|''${WHO}[[:space:]]" || true
        return 0
      fi

      rm -f "$PID_FILE"
      echo "not awake"
      return 1
    }

    case "''${1:-toggle}" in
      start) start ;;
      stop) stop ;;
      status) status ;;
      toggle)
        if is_running; then
          stop
        else
          start
        fi
        ;;
      -h|--help|help) usage ;;
      *)
        usage
        exit 2
        ;;
    esac
  '';
in
{
  home.packages = [
    git-cleanup
    git-cm
  ]
  ++ lib.optionals stdenv.isLinux [
    awake
  ];

  programs.bash = {
    enable = true;
    shellAliases = aliases;
    initExtra = ''
      __storage_costs_after_nh_switch() {
        local script="$HOME/.dotfiles/scripts/nix-storage-costs"
        if [ ! -x "$script" ]; then
          printf '\nStorage-cost callback skipped: %s is not executable\n' "$script" >&2
          return 0
        fi

        "$script" --after-nh-switch "$@"
      }

      nh() {
        command nh "$@"
        local nh_status=$?

        if [ "$nh_status" -eq 0 ] && [ "$#" -ge 2 ]; then
          case "$1 $2" in
            "os switch"|"home switch")
              __storage_costs_after_nh_switch "$@" || echo "Storage-cost report failed" >&2
              ;;
          esac
        fi

        return "$nh_status"
      }
    '';
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

            # Ensure ~/.local/bin is in PATH
            if not contains "$HOME/.local/bin" $PATH
              set -gx PATH "$HOME/.local/bin" $PATH
            end
          ''
        else
          ''
            # Ensure ~/.local/bin is in PATH
            if not contains "$HOME/.local/bin" $PATH
              set -gx PATH "$HOME/.local/bin" $PATH
            end
          ''
      )
      + ''
        set -gx AWS_DEFAULT_REGION eu-west-2
        set -gx AWS_REGION eu-west-2
        set -gx MUX_BACKEND herdr
        set -gx ERL_AFLAGS "-kernel shell_history enabled"
        set -gx SAM_CLI_TELEMETRY 0
        complete -c gitignore -w git-ignore
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
        # Navigation must happen in Fish, not the git-cm child process, so the
        # caller moves into an existing default-branch worktree before cleanup.
        if test (count $argv) -eq 1; and test "$argv[1]" = cm
          set -l target (command git-cm --default-branch)
          or return $status
          git switch "$target"
          or return $status
          command git-cleanup
          return $status
        end

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

      __storage_costs_after_nh_switch = ''
        set -l script "$HOME/.dotfiles/scripts/nix-storage-costs"
        if not test -x "$script"
          echo
          echo "Storage-cost callback skipped: $script is not executable" >&2
          return 0
        end

        "$script" --after-nh-switch $argv
      '';

      nh = ''
        command nh $argv
        set -l nh_status $status

        if test $nh_status -eq 0; and test (count $argv) -ge 2
          switch "$argv[1] $argv[2]"
            case "os switch" "home switch"
              __storage_costs_after_nh_switch $argv
          end
        end

        return $nh_status
      '';

      gitignore = ''
        set -l templates

        for arg in $argv
          for template in (string split , -- "$arg")
            if test -n "$template"
              set -a templates "$template"
            end
          end
        end

        command git-ignore $templates
      '';
      __notes_iso_date =
        if stdenv.isDarwin then
          ''
            switch "$argv[1]"
              case today
                date +"%Y-%m-%d"
              case yesterday
                date -v-1d +"%Y-%m-%d"
              case tomorrow
                date -v+1d +"%Y-%m-%d"
              case '*'
                return 1
            end
          ''
        else
          ''
            switch "$argv[1]"
              case today
                date +"%Y-%m-%d"
              case yesterday
                date -d yesterday +"%Y-%m-%d"
              case tomorrow
                date -d tomorrow +"%Y-%m-%d"
              case '*'
                return 1
            end
          '';
      today = "notes_on (__notes_iso_date today) today.md";
      yesterday = "notes_on (__notes_iso_date yesterday) today.md";
      tomorrow = "notes_on (__notes_iso_date tomorrow) today.md";
    };
  };
  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.package = pkgs.zoxide;
}
