{
  lib,
  pkgs,
  ...
}:
let
  vimPlugins = pkgs.vimPlugins;

  buildPlugin =
    {
      pname,
      owner,
      repo,
      rev,
      hash,
    }:
    pkgs.vimUtils.buildVimPlugin {
      inherit pname;
      version = rev;
      src = pkgs.fetchFromGitHub {
        inherit
          owner
          repo
          rev
          hash
          ;
      };
    };

  customPlugins = {
    presenting-nvim = buildPlugin {
      pname = "presenting.nvim";
      owner = "sotte";
      repo = "presenting.nvim";
      rev = "e78245995a09233e243bf48169b2f00dc76341f7";
      hash = "sha256-Q/SNFkMSREVEeDiikdMXQCVxrt3iThQUh08YMcN9qSk=";
    };
    vim-textobj-python = buildPlugin {
      pname = "vim-textobj-python";
      owner = "bps";
      repo = "vim-textobj-python";
      rev = "06de233e805b6bcfd0fde7591c64cf927637feb7";
      hash = "sha256-zR/eaSFS+0Lo1bQtioTbiCEzJ2Y5cMt0K9HtMfeqqao=";
    };
    vim-textobj-ruby = buildPlugin {
      pname = "vim-textobj-ruby";
      owner = "tek";
      repo = "vim-textobj-ruby";
      rev = "e955714b2e8cd581a4945b5ac5428c4c18de1657";
      hash = "sha256-JFMgjdXMl5ssOdeeLKuienzeoXwdQmjKp+NKUsX+0Zk=";
    };
    vim-textobj-keyvalue = buildPlugin {
      pname = "vim-textobj-keyvalue";
      owner = "vimtaku";
      repo = "vim-textobj-keyvalue";
      rev = "77bdefa7a737274a44fbdbd4eefa269dab9cf3bb";
      hash = "sha256-0BulWBvLJ5ejUKUP0u1TPdjLLn0ST6fmV1vBI6UwrQ8=";
    };
    vim-textobj-erb = buildPlugin {
      pname = "vim-textobj-erb";
      owner = "whatyouhide";
      repo = "vim-textobj-erb";
      rev = "03e38404e6f728289da14417204b731d9c19ea72";
      hash = "sha256-mqPmmQPd17/twz+28U7A1tV0/PFn5x+yDwU8qMurEjw=";
    };
    vim-textobj-xmlattr = buildPlugin {
      pname = "vim-textobj-xmlattr";
      owner = "whatyouhide";
      repo = "vim-textobj-xmlattr";
      rev = "694a297f1d75fd527e87da9769f3c6519a87ebb1";
      hash = "sha256-+91FVP95oh00flINdltqx6qJuijYo56tHIh3J098G2Q=";
    };
  };

  pluginMap = {
    "tpope/vim-sleuth" = vimPlugins.vim-sleuth;
    "andymass/vim-matchup" = vimPlugins.vim-matchup;
    "christoomey/vim-sort-motion" = vimPlugins.vim-sort-motion;
    "lepture/vim-jinja" = vimPlugins.vim-jinja;
    "stefandtw/quickfix-reflector.vim" = vimPlugins.quickfix-reflector-vim;
    "tpope/vim-dispatch" = vimPlugins.vim-dispatch;
    "tpope/vim-rhubarb" = vimPlugins.vim-rhubarb;
    "tpope/vim-unimpaired" = vimPlugins.vim-unimpaired;
    "xiyaowong/nvim-cursorword" = vimPlugins.vim-cursorword;
    "github/copilot.vim" = vimPlugins.copilot-vim;
    "b4winckler/vim-angry" = vimPlugins.vim-angry;
    "bps/vim-textobj-python" = customPlugins.vim-textobj-python;
    "kana/vim-textobj-line" = vimPlugins.vim-textobj-line;
    "kana/vim-textobj-user" = vimPlugins.vim-textobj-user;
    "michaeljsmith/vim-indent-object" = vimPlugins.vim-indent-object;
    "tek/vim-textobj-ruby" = customPlugins.vim-textobj-ruby;
    "vimtaku/vim-textobj-keyvalue" = customPlugins.vim-textobj-keyvalue;
    "whatyouhide/vim-textobj-erb" = customPlugins.vim-textobj-erb;
    "whatyouhide/vim-textobj-xmlattr" = customPlugins.vim-textobj-xmlattr;
    "sotte/presenting.nvim" = customPlugins.presenting-nvim;
    "kylechui/nvim-surround" = vimPlugins.nvim-surround;
    "folke/noice.nvim" = vimPlugins.noice-nvim;
    "rcarriga/nvim-notify" = vimPlugins.nvim-notify;
    "MunifTanjim/nui.nvim" = vimPlugins.nui-nvim;
    "lewis6991/gitsigns.nvim" = vimPlugins.gitsigns-nvim;
    "vim-test/vim-test" = vimPlugins.vim-test;
    "NvChad/nvim-colorizer.lua" = vimPlugins.nvim-colorizer-lua;
    "Wansmer/treesj" = vimPlugins.treesj;
    "nvim-tree/nvim-tree.lua" = vimPlugins.nvim-tree-lua;
    "nvim-tree/nvim-web-devicons" = vimPlugins.nvim-web-devicons;
    "christoomey/vim-tmux-navigator" = vimPlugins.vim-tmux-navigator;
    "tpope/vim-fugitive" = vimPlugins.vim-fugitive;
    "folke/flash.nvim" = vimPlugins.flash-nvim;
    "nvim-telescope/telescope.nvim" = vimPlugins.telescope-nvim;
    "nvim-lua/plenary.nvim" = vimPlugins.plenary-nvim;
    "nvim-telescope/telescope-github.nvim" = vimPlugins.telescope-github-nvim;
    "nvim-telescope/telescope-live-grep-args.nvim" = vimPlugins.telescope-live-grep-args-nvim;
    "nvim-telescope/telescope-fzf-native.nvim" = vimPlugins.telescope-fzf-native-nvim;
    "nvim-telescope/telescope-ui-select.nvim" = vimPlugins.telescope-ui-select-nvim;
    "neovim/nvim-lspconfig" = vimPlugins.nvim-lspconfig;
    "saghen/blink.cmp" = vimPlugins.blink-cmp;
    "antosha417/nvim-lsp-file-operations" = vimPlugins.nvim-lsp-file-operations;
    "j-hui/fidget.nvim" = vimPlugins.fidget-nvim;
    "stevearc/conform.nvim" = vimPlugins.conform-nvim;
    "rafamadriz/friendly-snippets" = vimPlugins.friendly-snippets;
    "rose-pine/neovim" = vimPlugins.rose-pine;
    "rose-pine" = vimPlugins.rose-pine;
    "folke/todo-comments.nvim" = vimPlugins.todo-comments-nvim;
    "echasnovski/mini.nvim" = vimPlugins.mini-nvim;
    "nvim-treesitter/nvim-treesitter" = vimPlugins.nvim-treesitter;
  };

  optionalPluginNames = [
    "sotte/presenting.nvim"
    "kylechui/nvim-surround"
    "folke/noice.nvim"
    "rcarriga/nvim-notify"
    "MunifTanjim/nui.nvim"
    "NvChad/nvim-colorizer.lua"
    "Wansmer/treesj"
    "nvim-tree/nvim-tree.lua"
    "nvim-tree/nvim-web-devicons"
    "folke/flash.nvim"
    "nvim-telescope/telescope.nvim"
    "nvim-lua/plenary.nvim"
    "nvim-telescope/telescope-github.nvim"
    "nvim-telescope/telescope-live-grep-args.nvim"
    "nvim-telescope/telescope-fzf-native.nvim"
    "nvim-telescope/telescope-ui-select.nvim"
  ];

  startPluginNames = lib.subtractLists optionalPluginNames (builtins.attrNames pluginMap);

  startPlugins = map (name: pluginMap.${name}) startPluginNames;
  optionalPlugins = map (name: {
    plugin = pluginMap.${name};
    optional = true;
  }) optionalPluginNames;
in
{
  programs.neovim = {
    enable = true;
    defaultEditor = true;
    viAlias = true;
    vimAlias = true;
    vimdiffAlias = true;
    withNodeJs = true;
    withPython3 = true;
    withRuby = false;
    initLua = builtins.readFile ../config/nvim/init.lua;

    plugins = startPlugins ++ optionalPlugins;

    extraPackages =
      with pkgs;
      [
        bash-language-server
        black
        ccls
        eslint
        fd
        fish
        gopls
        isort
        lua-language-server
        marksman
        nil
        nodejs_latest
        prettier
        pyright
        ripgrep
        ruby-lsp
        stylua
        terraform-ls
        tree-sitter
        typescript-language-server
        vscode-langservers-extracted
        xdg-utils
      ]
      ++ lib.optionals pkgs.stdenv.isLinux [
        wl-clipboard
        xclip
      ];
  };

  home.sessionVariables = {
    NIX_NEOVIM = "1";
  };
}
