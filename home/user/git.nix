{ pkgs, ... }:
let
  gitWithWorktreeDirenv = pkgs.writeShellScriptBin "git" ''
    set -euo pipefail

    real_git="${pkgs.git}/bin/git"

    allow_worktree_direnv() {
      worktree_path="$1"

      [ -f "$worktree_path/flake.nix" ] || return 0

      printf 'use flake . --impure\n' > "$worktree_path/.envrc"

      if command -v direnv >/dev/null 2>&1; then
        (cd "$worktree_path" && direnv allow)
      else
        echo "direnv not found; created $worktree_path/.envrc but did not allow it" >&2
      fi
    }

    if [ "$#" -ge 3 ] && [ "$1" = "worktree" ] && [ "$2" = "add" ]; then
      before="$("$real_git" worktree list --porcelain | sed -n 's/^worktree //p')"

      set +e
      "$real_git" "$@"
      git_status=$?
      set -e

      [ "$git_status" -eq 0 ] || exit "$git_status"

      after="$("$real_git" worktree list --porcelain | sed -n 's/^worktree //p')"

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
