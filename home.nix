{ config, pkgs, ... }:

let
  aliases = {
    ag = "rg";
    cd = "z";
    cdr = "cd ~/Repositories";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    ll = "ls -l";
    la = "ls -la";
    vim = "nvim";
    vi = "nvim";
    vimdiff = "nvim -d";
  };
  abbreviations = {
    ag = "rg";
    cd = "z";
    cdr = "cd ~/Repositories";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    rc = "bundle exec rails console";
    rs = "bundle exec rails server";
    rr = "bundle exec rails routes --expanded";
    sk = "bundle exec sidekiq";
    t = "bundle exec rspec --format p";
    tg = "terragrunt";
    pbcopy = "xclip -selection clipboard";
    pbpaste = "xclip -selection clipboard -o";
    g = "git";
  };
in
{
  home.username = "william";
  home.homeDirectory = "/home/william";

  home.stateVersion = "23.11"; # Please read the comment before changing.

  home.packages = with pkgs; [
    asdf-vm
    bat
    delta
    fd
    firefox
    gh
    gnumake
    golangci-lint 
    hclfmt
    kitty
    lsof
    markdownlint-cli
    neofetch
    nodejs
    pre-commit
    ripgrep
    shellharden
    shfmt
    telegram-desktop
    tmux
    tmuxinator
    xclip
    yarn
    zip
    zoxide
  ];

  # Add node packages
  # home.nodejs = {
  #   version = "21.7.3";
  #   packages = with pkgs.nodePackages; [
  #     typescript
  #     yarn
  #     prettier
  #     eslint
  #     stylelint
  #     markdownlint-cli
  #   ];
  # };

  # Home Manager is pretty good at managing dotfiles. The primary way to manage
  # plain files is through 'home.file'.
  home.file = {
    # # Building this configuration will create a copy of 'dotfiles/screenrc' in
    # # the Nix store. Activating the configuration will then make '~/.screenrc' a
    # # symlink to the Nix store copy.
    # ".screenrc".source = dotfiles/screenrc;

    # # You can also set the file content immediately.
    # ".gradle/gradle.properties".text = ''
    #   org.gradle.console=verbose
    #   org.gradle.daemon.idletimeout=3600000
    # '';
  };

  home.sessionVariables = {
    # VISUAL = "nvim";
    EDITOR = "nvim";
    ASDF_RUBY_BUILD_VERSION = "master";
    LESS = "-R";
    GIT_PAGER = "delta";
    MANPAGER = "nvim +Man!";
    PAGER = "less --raw-control-chars -F -X";
    RUBYOPT = "--enable-yjit";
    fish_greeting = "";
  };

  # Let Home Manager install and manage itself.
  programs.home-manager.enable = true;

  programs.bash = {
    enable = true;
    shellAliases = aliases;
  };

  programs.fish = {
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;
    # plugins = with pkgs.fishermanPackages; [
    #   bass
    #   fzf
    #   fzf_key_bindings
    #   fzf_files
    #   fzf_git
    #   fzf_history
    #   fzf_process
    #   fzf_pure
    #   fzf_ssh
    #   fzf_url
    #   fzf_vim
    #   fzf_z
    #   fzf_zoxide
    #   fzf_fd
    #   fzf_gh
    # ];
  };

  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.enableBashIntegration = true;

  programs.tmux = {
    enable = true;
    plugins = with pkgs.tmuxPlugins; [
      vim-tmux-navigator
      sensible 
      nord 
      resurrect
      yank
      tmux-thumbs
    ];
    extraConfig = (builtins.readFile ./tmux/tmux.conf);
  };

  programs.git =  {
    enable = true;
    userName = "William Fish";
    userEmail = "william.michael.fish@gmail.com";
    delta = {
      enable = true;
    };
    extraConfig = {
      core = {
        editor = "nvim";
      };
      delta = {
        navigate = true;
        light = false;
        features = "line-numbers decorations";
        theme = "Github";
      };
      user = {
        name = "William Fish";
        email = "william.michael.fish@gmail.com";
      };
      push = {
        default = "simple";
      };
      alias = {
        add = "git add -p";
        branches = "for-each-ref --sort=-committerdate --format=\"%(color:blue)%(authordate:relative)\t%(color:red)%(authorname)\t%(color:white)%(color:bold)%(refname:short)\" refs/remotes";
        cleanupmaster = "!git fetch -p && git pull && git branch --merged | grep -v main | xargs -n 1 -r git branch -d";
        cleanup = "!git fetch -p && git pull && git branch --merged | grep -v main | xargs -n 1 -r git branch -d";
        cmm = "!git checkout master && git cleanupmaster";
        cm = "!git checkout main && git cleanup";
      };
      help = {
        autocorrect = 1;
      };
      filter = {
        lfs = {
          clean = "git-lfs clean -- %f";
          smudge = "git-lfs smudge -- %f";
          process = "git-lfs filter-process";
          required = true;
        };
      };
      github = {
        user = "willfish";
      };
      web = {
        browser = "firefox";
      };
      init = {
        defaultBranch = "main";
      };
      credential = {
        "https://github.com" = {
          helper = "!/usr/bin/gh auth git-credential";
        };
        "https://gist.github.com" = {
          helper = "!/usr/bin/gh auth git-credential";
        };
      };
      merge = {
        conflictstyle = "diff3";
      };
      diff = {
        colorMoved = "default";
      };
    };
  };


  programs.tmux.tmuxinator.enable = true;

  programs.neovim = 
  let
    toLua = str: "lua << EOF\n${str}\nEOF\n";
    toLuaFile = file: "luafile " + file;
  in
  {
    enable = true;
    withRuby = true;
    withNodeJs = true;
    defaultEditor = true;

    plugins = with pkgs.vimPlugins;[
      {
        plugin = rose-pine;
        config = ''
          let g:rose_pine_variant = 'moon'
          colorscheme rose-pine
        '';
      }
      nvim-lspconfig
      neodev-nvim
      mason-nvim
      neodev-nvim
      {
        plugin = nvim-cmp;
        config = toLuaFile ./nvim/nvim-cmp.lua;
      }
      cmp_luasnip
      cmp-nvim-lsp
      cmp-buffer
      cmp-path
      cmp-cmdline
      cmp-git
      ultisnips
      cmp-nvim-ultisnips
      copilot-vim
      vim-snippets
      { 
        plugin = telescope-nvim;
        config = toLuaFile ./nvim/telescope.lua;
      }
      telescope-fzf-native-nvim
      telescope-github-nvim
      telescope-live-grep-args-nvim
      luasnip
      lualine-nvim
      nvim-web-devicons
      {
        plugin = nvim-tree-lua;
        config = toLuaFile ./nvim/nvim-tree.lua;
      }
      {
        plugin = null-ls-nvim;
        config = toLuaFile ./nvim/null-ls.lua;
      }
      (nvim-treesitter.withPlugins (p: [
        p.tree-sitter-nix
        p.tree-sitter-vim
        p.tree-sitter-bash
        p.tree-sitter-lua
        p.tree-sitter-python
        p.tree-sitter-json
        p.tree-sitter-yaml
        p.tree-sitter-ruby
        p.tree-sitter-typescript
        p.tree-sitter-javascript
        p.tree-sitter-go
        p.tree-sitter-rust
        p.tree-sitter-fish
      ]))

      vim-nix
      vim-go
      nvim-treesitter-textsubjects
      nvim-treesitter-endwise
      vim-textobj-user
      # {
      #   src = pkgs.fetchFromGitHub {
      #     owner = "tek";
      #     repo = "vim-textobj-ruby";
      #     rev = "master";
      #     sha256 = "";
      #   };
      # }
      vim-indent-object
      vim-sort-motion
      # {
      #   src = pkgs.fetchFromGitHub {
      #     owner = "Wansmer";
      #     repo = "treesj";
      #     rev = "main";
      #     sha256 = "";
      #   };
      #   config = toLuaFile ./nvim/treesj.lua;
      # }
      vim-sort-motion
      vim-matchup
      nvim-surround
      vim-dispatch
      {
        plugin = vim-test;
        config = toLuaFile ./nvim/vim-test.lua;
      }
      {
        plugin = vim-fugitive;
        config = toLuaFile ./nvim/fugitive.lua;
      }
      vim-rhubarb
      quickfix-reflector-vim
      vim-commentary
      vim-unimpaired
      { 
        plugin = vim-tmux-navigator;
        config = toLuaFile ./nvim/vim-tmux-navigator.lua;
      }
      vim-tmux
    ];
  };
}
