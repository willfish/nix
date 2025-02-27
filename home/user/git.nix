{
  programs.git =  {
    enable = true;
    userName = "William Fish";
    userEmail = "william.michael.fish@gmail.com";
    extraConfig = {
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
        gpgSign = true;
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
      github.user = "willfish";
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
        add = "git add -p";
        branches = "for-each-ref --sort=-committerdate --format=\"%(color:blue)%(authordate:relative)\t%(color:red)%(authorname)\t%(color:white)%(color:bold)%(refname:short)\" refs/remotes";
        cleanup = "!git fetch -p && git pull && git branch --merged | grep -v main | xargs -n 1 -r git branch -d";
        cm = "!git checkout main && git cleanup";
      };

      filter = {
        lfs = {
          clean = "git-lfs clean -- %f";
          smudge = "git-lfs smudge -- %f";
          process = "git-lfs filter-process";
          required = true;
        };
      };

      credential = {
        "https://github.com" = {
          helper = "!/usr/bin/gh auth git-credential";
        };
        "https://gist.github.com" = {
          helper = "!/usr/bin/gh auth git-credential";
        };
      };
    };
  };
}
