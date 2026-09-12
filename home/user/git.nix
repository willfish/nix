{
  config,
  lib,
  pkgs,
  ...
}:
let
  gitWithWorktreeDirenv = pkgs.writeShellScriptBin "git" ''
    set -euo pipefail

    real_git="${pkgs.git}/bin/git"

    bootstrap_tracked_worktree_direnv() {
      worktree_path="$1"

      [ -f "$worktree_path/flake.nix" ] || return 0
      # Trust belongs to direnv. The git wrapper only warms tracked .envrc files
      # that the user has already allowed; it never creates or allows one.
      "$real_git" -C "$worktree_path" ls-files --error-unmatch .envrc >/dev/null 2>&1 || return 0
      [ ! -e "$worktree_path/.pre-commit-config-nix.yaml" ] || return 0

      if command -v direnv >/dev/null 2>&1; then
        if ! (cd "$worktree_path" && direnv exec . true); then
          echo "direnv is not allowed for $worktree_path; inspect .envrc and run direnv allow if desired" >&2
        fi
      else
        echo "direnv not found; skipping bootstrap for $worktree_path" >&2
      fi
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

    current_repo_root() {
      dir="$(command_working_dir)"
      "$real_git" -c core.fsmonitor=false -C "$dir" rev-parse --show-toplevel 2>/dev/null || true
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
        if ! ${pkgs.postgresql}/bin/pg_ctl stop -D "$pgdata" -s -m fast; then
          echo "Git completed, but PostgreSQL did not stop; retaining state in $pgdata" >&2
          return 1
        fi
      fi

      rm -f "$pidfile"
      rm -rf "$pghost" \
        "$pgdata" \
        "$HOME/.local/share/gem/worktrees/$worktree_id" \
        "$HOME/.cache/bundle/worktrees/$worktree_id"
    }

    cleanup_removed_worktree_state() {
      local before_file="$1" after_file="$2"
      local field remaining_field worktree_path worktree_id registered

      while IFS= read -r -d "" field; do
        case "$field" in
          worktree\ *) worktree_path="''${field#worktree }" ;;
          *) continue ;;
        esac

        registered=0
        while IFS= read -r -d "" remaining_field; do
          if [ "$remaining_field" = "worktree $worktree_path" ]; then
            registered=1
            break
          fi
        done < "$after_file"
        [ "$registered" -eq 0 ] || continue
        # Retain state if the path was recreated while Git was running.
        [ ! -e "$worktree_path" ] && [ ! -L "$worktree_path" ] || continue

        worktree_id="$(worktree_id_for_path_string "$worktree_path" || true)"
        [ -n "$worktree_id" ] || continue
        cleanup_worktree_state_by_id "$worktree_id" || return 1
      done < "$before_file"
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
      && { [ "''${args[$((subcommand_index + 1))]}" = "remove" ] \
        || [ "''${args[$((subcommand_index + 1))]}" = "prune" ]; }; then
      snapshot_dir="$(mktemp -d)"
      trap 'rm -rf -- "$snapshot_dir"' EXIT
      trap 'exit 130' INT
      trap 'exit 143' TERM
      "$real_git" "''${global_args[@]}" worktree list --porcelain -z > "$snapshot_dir/before"

      # Let Git validate flags, dirty/locked worktrees, expiry and dry runs.
      # No service state is touched until Git has actually removed a record.
      if "$real_git" "$@"; then
        "$real_git" "''${global_args[@]}" worktree list --porcelain -z > "$snapshot_dir/after"
        cleanup_removed_worktree_state "$snapshot_dir/before" "$snapshot_dir/after"
        exit 0
      else
        exit "$?"
      fi
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
          bootstrap_tracked_worktree_direnv "$worktree_path"
        fi
      done

      exit "$git_status"
    fi

    if [ "$#" -gt "$subcommand_index" ] \
      && { [ "''${args[$subcommand_index]}" = "commit" ] \
        || [ "''${args[$subcommand_index]}" = "push" ]; }; then
      repo_root="$(current_repo_root)"
      if [ -n "$repo_root" ]; then
        bootstrap_tracked_worktree_direnv "$repo_root"
      fi
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
      web.browser = lib.mkIf config.dotfiles.capabilities.desktop "brave";
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
        graph = "log --graph --all --decorate --date=relative --pretty=format:'%C(bold cyan)%h%C(reset) %C(bold yellow)%d%C(reset) %C(white)%s%C(reset) %C(dim white)- %an, %ar%C(reset)'";
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
