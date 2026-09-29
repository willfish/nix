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
    cat = "bat";
    la = "lsd -la";
    ll = "lsd -l";
    v = "nvim";
    vi = "nvim";
    vim = "nvim";
    vimdiff = "nvim -d";
  }
  // lib.optionalAttrs config.dotfiles.capabilities.work { psql = "pgcli"; };
  abbreviations = {
    a = "herdr";
    ag = "rg";

    cdn = "cd ~/Notes";
    cdr = "cd ~/Repositories";
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

    # Prune administrative records only. A missing checkout may still be
    # unpublished, so this raw prune must not delete its service state.
    ${pkgs.git}/bin/git worktree prune

    subjects_file="$(mktemp)"
    trap 'rm -f "$subjects_file"' EXIT
    git log "$default_ref" --format=%s | sed -E 's/ \(#[0-9]+\)$//' | sort -u > "$subjects_file"

    subject_on_default() {
      local subject="$1"
      subject="$(printf '%s\n' "$subject" | sed -E 's/ \(#[0-9]+\)$//')"
      [ -n "$subject" ] || return 1
      grep -qxF -e "$subject" "$subjects_file"
    }

    # Landed means every commit not in the default branch has the same
    # normalized subject as a default-branch commit. That covers a real merge
    # and a squash whose hashes are no longer ancestors.
    history_landed() {
      local rev="$1" subject
      while IFS= read -r subject || [ -n "$subject" ]; do
        subject_on_default "$subject" || return 1
      done < <(git log --format=%s "$rev" --not "$default_ref")
    }

    no_live_upstream() {
      local branch="$1" upstream
      [ -n "$branch" ] || return 0
      upstream="$(git for-each-ref --format='%(upstream)' "refs/heads/$branch")" || return 1
      case "$upstream" in
        refs/remotes/origin/*)
          if git show-ref --verify --quiet "$upstream"; then
            return 1
          fi
          ;;
      esac
      return 0
    }

    commits_only_here() {
      local rev="$1" branch="$2" obj name
      local -a excludes=()
      while IFS=' ' read -r obj name; do
        [ -n "$obj" ] || continue
        if [ -n "$branch" ] && [ "$name" = "refs/heads/$branch" ]; then
          continue
        fi
        excludes+=("$obj")
      done < <(git for-each-ref --format='%(objectname) %(refname)' refs/heads refs/remotes refs/tags)
      excludes+=("$default_ref")
      git rev-list --count "$rev" --not "''${excludes[@]}"
    }

    checkout_landed() {
      local rev="$1" branch="$2"
      [ -z "$branch" ] || [ "$branch" != "$default" ] || return 1
      no_live_upstream "$branch" || return 1
      if history_landed "$rev"; then
        return 0
      fi
      # A detached or duplicate checkout can go when its tip message is already
      # on the default branch and another ref still holds the old history.
      subject_on_default "$(git log -1 --format=%s "$rev")" || return 1
      [ "$(commits_only_here "$rev" "$branch")" -eq 0 ]
    }

    blocking_dirt() {
      local wt="$1" line code path
      while IFS= read -r line; do
        [ -n "$line" ] || continue
        code="''${line:0:2}"
        path="''${line:3}"
        case "$code" in
          '!!')
            case "$path" in
              .envrc|*/.envrc) return 0 ;;
            esac
            ;;
          ' D'|'D ')
            git -C "$wt" ls-files -v -- "$path" | grep -q '^[HS]' || return 0
            ;;
          *)
            return 0
            ;;
        esac
      done < <(git -C "$wt" status --porcelain --untracked-files=all --ignored)
      return 1
    }

    remove_if_safe() {
      local wt="$1" rev="$2" branch="$3" label="$branch"
      [ "$wt" != "$current_root" ] && [ -d "$wt" ] || return 0
      checkout_landed "$rev" "$branch" || return 0
      if blocking_dirt "$wt"; then
        return 0
      fi
      [ -n "$label" ] || label="detached"

      # Force only after the dirt filter. Git otherwise keeps assume-unchanged
      # deletions and generated ignored files, which are not local work.
      if git worktree remove "$wt" || git worktree remove --force "$wt"; then
        echo "Removed worktree: $wt ($label)"
        if [ -n "$branch" ]; then
          if history_landed "refs/heads/$branch"; then
            git branch -D "$branch" || echo "Branch $branch retained by Git"
          else
            echo "Branch $branch retained; its history is not on $default"
          fi
        fi
      else
        echo "Could not remove worktree $wt; retained remaining state" >&2
        return 1
      fi
    }

    wt="" rev="" branch="" locked=0
    while IFS= read -r -d "" field; do
      case "$field" in
        worktree\ *) wt="''${field#worktree }" ;;
        HEAD\ *) rev="''${field#HEAD }" ;;
        branch\ refs/heads/*) branch="''${field#branch refs/heads/}" ;;
        detached) branch="" ;;
        locked|locked\ *) locked=1 ;;
        "")
          if [ "$locked" -eq 0 ] && [ -n "$rev" ]; then
            remove_if_safe "$wt" "$rev" "$branch"
          fi
          wt="" rev="" branch="" locked=0
          ;;
      esac
    done < <(git worktree list --porcelain -z)

    # Same landing rule for branches with no checkout. Git still refuses to
    # delete a branch that a retained worktree has checked out.
    while IFS= read -r branch; do
      [ "$branch" = "$default" ] && continue
      no_live_upstream "$branch" || continue
      history_landed "refs/heads/$branch" || continue
      git branch -D "$branch" 2>/dev/null || true
    done < <(git for-each-ref --format='%(refname:short)' refs/heads)
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
in
{
  home.packages = [
    git-cleanup
    git-cm
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
