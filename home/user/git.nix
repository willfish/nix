{ pkgs, ... }:
let
  gitWithWorktreeDirenv = pkgs.writeShellScriptBin "git" ''
    set -euo pipefail

    real_git="${pkgs.git}/bin/git"

    allow_worktree_direnv() {
      worktree_path="$1"

      [ -f "$worktree_path/flake.nix" ] || return 0

      if ! "$real_git" -C "$worktree_path" ls-files --error-unmatch .envrc >/dev/null 2>&1; then
        printf 'use flake . --impure\n' > "$worktree_path/.envrc"
      fi

      if command -v direnv >/dev/null 2>&1; then
        (cd "$worktree_path" && direnv allow)
      else
        echo "direnv not found; created $worktree_path/.envrc but did not allow it" >&2
      fi
    }

    worktree_remove_target() {
      remove_index="$1"
      target=""
      target_index=$((remove_index + 2))
      arg_count="''${#args[@]}"

      while [ "$target_index" -lt "$arg_count" ]; do
        arg="''${args[$target_index]}"

        case "$arg" in
          --)
            target_index=$((target_index + 1))
            if [ "$target_index" -lt "$arg_count" ]; then
              target="''${args[$target_index]}"
            fi
            break
            ;;
          -*)
            target_index=$((target_index + 1))
            ;;
          *)
            target="$arg"
            break
            ;;
        esac
      done

      printf '%s\n' "$target"
    }

    command_working_dir() {
      dir="$PWD"
      global_index=0

      while [ "$global_index" -lt "$subcommand_index" ]; do
        arg="''${args[$global_index]}"

        case "$arg" in
          -C)
            global_index=$((global_index + 1))
            if [ "$global_index" -lt "$subcommand_index" ]; then
              next_dir="''${args[$global_index]}"
              case "$next_dir" in
                /*)
                  dir="$next_dir"
                  ;;
                *)
                  dir="$dir/$next_dir"
                  ;;
              esac
            fi
            ;;
        esac

        global_index=$((global_index + 1))
      done

      (cd "$dir" && pwd -P)
    }

    worktree_path_for_remove_target() {
      target="$1"

      [ -n "$target" ] || return 0

      case "$target" in
        /*)
          printf '%s\n' "$target"
          ;;
        *)
          printf '%s/%s\n' "$(command_working_dir)" "$target"
          ;;
      esac
    }

    worktree_id_for_path() {
      worktree_path="$1"
      worktree_root="$(
        "$real_git" -c core.fsmonitor=false -C "$worktree_path" \
          rev-parse --show-toplevel 2>/dev/null || true
      )"

      [ -n "$worktree_root" ] || return 1

      if command -v md5sum >/dev/null 2>&1; then
        printf '%s\n' "$worktree_root" | md5sum | cut -c1-8
      elif command -v md5 >/dev/null 2>&1; then
        printf '%s\n' "$worktree_root" | md5 | cut -c1-8
      else
        return 1
      fi
    }

    worktree_id_for_path_string() {
      worktree_path="$1"

      if command -v md5sum >/dev/null 2>&1; then
        printf '%s\n' "$worktree_path" | md5sum | cut -c1-8
      elif command -v md5 >/dev/null 2>&1; then
        printf '%s\n' "$worktree_path" | md5 | cut -c1-8
      else
        return 1
      fi
    }

    cleanup_state_exists_for_worktree_id() {
      worktree_id="$1"

      [ -e "$HOME/.local/share/postgres/worktrees/$worktree_id/.worktree-initialized" ] && return 0
      [ -e "$HOME/.local/share/postgres/worktrees/$worktree_id" ] && return 0
      [ -e "/tmp/pg-$worktree_id" ] && return 0
      [ -e "/tmp/pg-$worktree_id.pid" ] && return 0
      [ -e "$HOME/.local/share/yarn/worktrees/$worktree_id/.worktree-initialized" ] && return 0
      [ -e "$HOME/.local/share/gem/worktrees/$worktree_id/.worktree-initialized" ] && return 0
      [ -e "$HOME/.local/share/gem/worktrees/$worktree_id" ] && return 0
      [ -e "$HOME/.cache/bundle/worktrees/$worktree_id" ] && return 0

      return 1
    }

    cleanup_worktree_state_by_id() {
      worktree_id="$1"

      cleanup_state_exists_for_worktree_id "$worktree_id" || return 0

      pgdata="$HOME/.local/share/postgres/worktrees/$worktree_id"
      pghost="/tmp/pg-$worktree_id"
      pidfile="/tmp/pg-$worktree_id.pid"

      echo "Cleaning stale worktree state $worktree_id" >&2

      if [ -d "$pgdata" ] && { [ -f "$pgdata/postmaster.pid" ] || [ -f "$pidfile" ]; }; then
        ${pkgs.postgresql}/bin/pg_ctl stop -D "$pgdata" -s -m fast || true
      fi

      rm -f "$pidfile"
      rm -rf "$pghost" \
        "$pgdata" \
        "$HOME/.local/share/gem/worktrees/$worktree_id" \
        "$HOME/.cache/bundle/worktrees/$worktree_id"
    }

    worktree_has_cleanup_state() {
      worktree_id="$1"

      cleanup_state_exists_for_worktree_id "$worktree_id"
    }

    cleanup_worktree_before_remove() {
      worktree_path="$1"

      [ -n "$worktree_path" ] || return 0
      [ -d "$worktree_path" ] || return 0
      [ -f "$worktree_path/flake.nix" ] || return 0

      if ! command -v direnv >/dev/null 2>&1; then
        echo "direnv not found; skipping worktree-clean for $worktree_path" >&2
        return 0
      fi

      worktree_id="$(worktree_id_for_path "$worktree_path" || true)"
      if [ -z "$worktree_id" ]; then
        echo "Could not determine worktree id; skipping worktree-clean for $worktree_path" >&2
        return 0
      fi

      # Only enter direnv when this worktree has already been initialised. Without
      # this guard, cleanup could accidentally trigger first-time setup.
      worktree_has_cleanup_state "$worktree_id" || return 0

      echo "Running worktree-clean before removing $worktree_path" >&2
      if ! (cd "$worktree_path" && direnv exec . worktree-clean); then
        echo "worktree-clean failed for $worktree_path; not removing worktree" >&2
        return 1
      fi
    }

    worktree_prune_is_dry_run() {
      prune_index="$1"
      target_index=$((prune_index + 2))
      arg_count="''${#args[@]}"

      while [ "$target_index" -lt "$arg_count" ]; do
        case "''${args[$target_index]}" in
          -n|--dry-run)
            return 0
            ;;
        esac

        target_index=$((target_index + 1))
      done

      return 1
    }

    cleanup_missing_worktrees_before_prune() {
      "$real_git" "''${global_args[@]}" worktree list --porcelain | sed -n 's/^worktree //p' | while IFS= read -r worktree_path; do
        [ -n "$worktree_path" ] || continue
        [ -d "$worktree_path" ] && continue

        worktree_id="$(worktree_id_for_path_string "$worktree_path" || true)"
        [ -n "$worktree_id" ] || continue

        cleanup_worktree_state_by_id "$worktree_id"
      done
    }

    args=("$@")
    subcommand_index=0
    while [ "$subcommand_index" -lt "$#" ]; do
      arg="''${args[$subcommand_index]}"
      case "$arg" in
        -C|-c|--git-dir|--work-tree|--namespace)
          subcommand_index=$((subcommand_index + 2))
          ;;
        --git-dir=*|--work-tree=*|--namespace=*|--exec-path=*|-c*)
          subcommand_index=$((subcommand_index + 1))
          ;;
        --)
          subcommand_index=$((subcommand_index + 1))
          break
          ;;
        -*)
          subcommand_index=$((subcommand_index + 1))
          ;;
        *)
          break
          ;;
      esac
    done

    global_args=("''${args[@]:0:$subcommand_index}")

    if [ "$#" -gt "$((subcommand_index + 1))" ] \
      && [ "''${args[$subcommand_index]}" = "worktree" ] \
      && [ "''${args[$((subcommand_index + 1))]}" = "remove" ]; then
      remove_target="$(worktree_remove_target "$subcommand_index")"
      cleanup_worktree_before_remove "$(worktree_path_for_remove_target "$remove_target")"
      exec "$real_git" "$@"
    fi

    if [ "$#" -gt "$((subcommand_index + 1))" ] \
      && [ "''${args[$subcommand_index]}" = "worktree" ] \
      && [ "''${args[$((subcommand_index + 1))]}" = "prune" ]; then
      if ! worktree_prune_is_dry_run "$subcommand_index"; then
        cleanup_missing_worktrees_before_prune
      fi

      exec "$real_git" "$@"
    fi

    if [ "$#" -gt "$((subcommand_index + 1))" ] \
      && [ "''${args[$subcommand_index]}" = "worktree" ] \
      && [ "''${args[$((subcommand_index + 1))]}" = "add" ]; then
      before="$("$real_git" "''${global_args[@]}" worktree list --porcelain | sed -n 's/^worktree //p')"

      set +e
      "$real_git" "$@"
      git_status=$?
      set -e

      [ "$git_status" -eq 0 ] || exit "$git_status"

      after="$("$real_git" "''${global_args[@]}" worktree list --porcelain | sed -n 's/^worktree //p')"

      printf '%s\n' "$after" | while IFS= read -r worktree_path; do
        [ -n "$worktree_path" ] || continue

        if ! printf '%s\n' "$before" | grep -Fxq "$worktree_path"; then
          allow_worktree_direnv "$worktree_path"
        fi
      done

      exit "$git_status"
    fi

    exec "$real_git" "$@"
  '';
in
{
  programs.git = {
    enable = true;
    package = gitWithWorktreeDirenv;
    signing = {
      key = "BC6DED9479D436F5";
      signByDefault = true;
    };
    settings = {
      user = {
        name = "William Fish";
        email = "william.michael.fish@gmail.com";
      };

      column.ui = "auto";
      branch.sort = "-committerdate";
      tag.sort = "version:refname";

      core = {
        editor = "nvim";
        excludesfile = "~/.gitignore_global";
        fsmonitor = true;
        untrackedCache = true;
        # Permanent equivalent of GIT_SSH_COMMAND workaround for the
        # "Bad owner or permissions" error on Nix store ssh_config.d files
        # (e.g. systemd-ssh-proxy.conf). -F /dev/null skips broken system
        # configs (no ~/.ssh/config is present anyway).
        sshCommand = "ssh -F /dev/null";
      };

      push = {
        default = "simple";
        autoSetupRemote = true;
        followTags = "true";
      };

      fetch = {
        prune = true;
        pruneTags = true;
        all = true;
      };

      commit = {
        status = true;
        verbose = true;
        template = "~/.gitmessage";
      };

      rebase = {
        autoSquash = true;
        autoStash = true;
        updateRefs = true;
      };

      pull.rebase = true;

      rerere = {
        enabled = true;
        autoupdate = true;
      };

      help.autocorrect = 1;
      web.browser = "brave";
      init.defaultBranch = "main";
      merge.conflictstyle = "zdiff3";

      diff = {
        algorithm = "histogram";
        colorMoved = "plain";
        mnemonicPrefix = true;
        renames = true;
      };

      delta = {
        navigate = true;
        light = false;
        features = "line-numbers decorations";
        theme = "Github";
      };

      alias = {
        add = "add -p";
        branches = "for-each-ref --sort=-committerdate --format=\"%(color:blue)%(authordate:relative)\t%(color:red)%(authorname)\t%(color:white)%(color:bold)%(refname:short)\" refs/remotes";
        taginfo = "for-each-ref --format='%(color:blue)%(refname:short) %(color:green)%(color:bold)%(taggerdate:short)%(committerdate:short)' --sort=committerdate refs/tags";
      };

      filter = {
        lfs = {
          clean = "git-lfs clean -- %f";
          smudge = "git-lfs smudge -- %f";
          process = "git-lfs filter-process";
          required = true;
        };
      };
    };
  };
}
