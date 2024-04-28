{ config, pkgs, pkgs-unstable, home-manager, ... }:

let
  aliases = {
    ag = "rg";
    cd = "z";
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
    hm = "cd ~/Repositories/hmrc";
    mux = "tmuxinator";
    tm = "tmux";
    a = "tmux attach";
    rc = "bundle exec rails console";
    rs = "bundle exec rails server";
    rr = "bundle exec rails routes --expanded";
    sk = "bundle exec sidekiq";
    t = "bundle exec rspec --format p";
    tg = "terragrunt";
    pbcopy = "wl-copy";
    pbpaste = "wl-paste";
    g = "git";
  };
in
{
  home.username = "william";
  home.homeDirectory = "/home/william";
  home.stateVersion = "23.11";

  home.packages = with pkgs; [
    nix-prefetch-git
    asdf-vm
    bat
    bat
    delta
    fd
    firefox
    fzf
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
    python3
    ripgrep
    rust-analyzer
    unzip
    shellharden
    shfmt
    telegram-desktop
    tmux
    tmuxinator
    tree
    yarn
    zip
    zoxide
    wofi
    pcmanfm
    hyprpaper
    pywal
    pyprland
    wezterm
    pavucontrol
    grim
    slurp
    libappindicator-gtk3
    awscli
    pavucontrol
    ruff

    # Wayland clipboard tooling
    wl-clipboard
    wl-clipboard-x11

    # Build tools
    bison
    flex
    fontforge
    makeWrapper
    pkg-config
    gnumake
    gcc
    libiconv
    autoconf
    automake
    libtool
    openssl

    # unfree
    # For zoom
    zoom-us
    glxinfo
    pulseaudioFull

    slack
    discord
    spotify
    steam
  ];

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
    XDG_CURRENT_DESKTOP = "gnome";
  };

  # Let Home Manager install and manage itself.
  programs.home-manager.enable = true;

  programs.bash = {
    enable = true;
    shellAliases = aliases;
    initExtra = ''
      source ${pkgs.asdf-vm}/share/asdf-vm/asdf.sh
    '';
  };

  programs.fish = {
    enable = true;
    shellAliases = aliases;
    shellAbbrs = abbreviations;
    shellInit = ''
      source ${pkgs.asdf-vm}/share/asdf-vm/asdf.fish
    '';
    plugins = with pkgs.fishermanPackages; [
      { name = "tide"; src = pkgs.fishPlugins.tide.src; }
    ];
  };

  programs.zoxide.enable = true;
  programs.zoxide.enableFishIntegration = true;
  programs.zoxide.enableBashIntegration = true;

  programs.tmux = {
    enable = true;
    plugins = with pkgs.tmuxPlugins; [
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
    delta.enable = true;
    extraConfig = {
      core.editor = "nvim";
      # user.name = "William Fish";
      # user.email = "william.michael.fish@gmail.com";
      push.default = "simple";
      help.autocorrect = 1;
      github.user = "willfish";
      web.browser = "firefox";
      init.defaultBranch = "main";
      merge.conflictstyle = "diff3";
      diff.colorMoved = "default";

      delta = {
        navigate = true;
        light = false;
        features = "line-numbers decorations";
        theme = "Github";
      };
      alias = {
        add = "git add -p";
        branches = "for-each-ref --sort=-committerdate --format=\"%(color:blue)%(authordate:relative)\t%(color:red)%(authorname)\t%(color:white)%(color:bold)%(refname:short)\" refs/remotes";
        cleanupmaster = "!git fetch -p && git pull && git branch --merged | grep -v main | xargs -n 1 -r git branch -d";
        cleanup = "!git fetch -p && git pull && git branch --merged | grep -v main | xargs -n 1 -r git branch -d";
        cmm = "!git checkout master && git cleanupmaster";
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


  programs.tmux.tmuxinator.enable = true;

  programs.kitty = {
    enable = true;
    extraConfig = (builtins.readFile ./kitty/kitty.conf);
  };

  programs.neovim =
  let
    toLua = str: "lua << EOF\n${str}\nEOF\n";
    toLuaFile = file: "luafile " + file;
  in
  {
    enable = true;
    withRuby = true;
    withPython3 = true;
    withNodeJs = true;
    defaultEditor = true;
    extraConfig = ''
      ${toLuaFile ./nvim/opts.lua}
      ${toLuaFile ./nvim/autocmds.lua}
      ${toLuaFile ./nvim/maps.lua}
    '';
    plugins = with pkgs.vimPlugins;[
      {
        plugin = rose-pine;
        config = ''
          let g:rose_pine_variant = 'moon'
          colorscheme rose-pine
        '';
      }
      {
        plugin = nvim-lspconfig;
        # config = toLuaFile ./nvim/nvim-lsp.lua;
      }
      neodev-nvim
      mason-nvim
      mason-lspconfig-nvim
      # {
      #   src = pkgs.fetchFromGitHub {
      #     owner = "williamboman";
      #     repo = "nvim-lsp-installer";
      #     rev = "main";
      #     sha256 = "";
      #   };
      # }
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
      vim-polyglot
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
      vim-indent-object
      vim-sort-motion
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
      # {
      #   src = pkgs.fetchFromGitHub {
      #     owner = "tek";
      #     repo = "vim-textobj-ruby";
      #     rev = "master";
      #     sha256 = "";
      #   };
      # }
      # {
      #   src = pkgs.fetchFromGitHub {
      #     owner = "Wansmer";
      #     repo = "treesj";
      #     rev = "main";
      #     sha256 = "";
      #   };
      #   config = toLuaFile ./nvim/treesj.lua;
      # }
    ];
  };
  programs.fzf.enable = true;

  programs.waybar = {
    enable = true;
    systemd.enable = true;
    style = ''
      ${builtins.readFile "${pkgs.waybar}/etc/xdg/waybar/style.css"}

      window#waybar {
        background: transparent;
        border-bottom: none;
      }
    '';
    settings = [{
      height = 30;
      layer = "top";
      position = "top";
      tray = { spacing = 10; };
      modules-left = [ "hyprland/workspaces" ];
      modules-center = [ "hyprland/window" ];
      modules-right = [
        "pulseaudio"
        "network"
        "cpu"
        "memory"
        "temperature"
        "clock"
        "tray"
      ];
      battery = {
        format = "{capacity}% {icon}";
        format-alt = "{time} {icon}";
        format-charging = "{capacity}% ";
        format-icons = [ "" "" "" "" "" ];
        format-plugged = "{capacity}% ";
        states = {
          critical = 15;
          warning = 30;
        };
      };
      clock = {
        format-alt = "{:%Y-%m-%d}";
        tooltip-format = "{:%Y-%m-%d | %H:%M}";
      };
      cpu = {
        format = "{usage}% ";
        tooltip = false;
      };
      memory = { format = "{}% "; };
      network = {
        interval = 1;
        format-alt = "{ifname}: {ipaddr}/{cidr}";
        format-disconnected = "Disconnected ⚠";
        format-ethernet = "{ifname}: {ipaddr}/{cidr}   up: {bandwidthUpBits} down: {bandwidthDownBits}";
        format-linked = "{ifname} (No IP) ";
        format-wifi = "{essid} ({signalStrength}%) ";
      };
      pulseaudio = {
        format = "{volume}% {icon} {format_source}";
        format-bluetooth = "{volume}% {icon} {format_source}";
        format-bluetooth-muted = " {icon} {format_source}";
        format-icons = {
          car = "";
          default = [ "" "" "" ];
          handsfree = "";
          headphones = "";
          headset = "";
          phone = "";
          portable = "";
        };
        format-muted = " {format_source}";
        format-source = "{volume}% ";
        format-source-muted = "";
        on-click = "pavucontrol";
      };
      temperature = {
        critical-threshold = 80;
        format = "{temperatureC}°C {icon}";
        format-icons = [ "" "" "" ];
      };
    }];
  };

  services.network-manager-applet.enable = true;
}
