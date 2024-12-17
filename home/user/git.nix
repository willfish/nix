{
  programs.git =  {
    enable = true;
    userName = "William Fish";
    userEmail = "william.michael.fish@gmail.com";
    extraConfig = {
      core.editor = "nvim";
      push.default = "simple";
      help.autocorrect = 1;
      github.user = "willfish";
      web.browser = "brave";
      init.defaultBranch = "main";
      merge.conflictstyle = "diff3";
      diff.colorMoved = "default";
      core.excludesfile = "~/.gitignore_global";

      delta = {
        navigate = true;
        light = false;
        features = "line-numbers decorations";
        theme = "Github";
      };
      alias = {
        add = "git add -p";
        branches = "for-each-ref --sort=-committerdate --format=\"%(color:blue)%(authordate:relative)\t%(color:red)%(authorname)\t%(color:white)%(color:bold)%(refname:short)\" refs/remotes";
        cleanup = "!git fetch -p && git pull && git branch --merged | grep -v master | xargs -n 1 -r git branch -d";
        cm = "!git checkout master && git cleanup";
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
