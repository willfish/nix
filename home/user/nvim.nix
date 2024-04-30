{ config, pkgs, ...}:
{
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
    # extraConfig = ''
    #   require("william.core")
    #   require("william.lazy")
    #   ${toLuaFile ../config/nvim/opts.lua}
    #   ${toLuaFile ../config/nvim/autocmds.lua}
    #   ${toLuaFile ../config/nvim/maps.lua}
    # '';
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
        # config = toLuaFile ./config/nvim/nvim-lsp.lua;
      }
      neodev-nvim
      mason-nvim
      mason-lspconfig-nvim
      neodev-nvim
      {
        plugin = nvim-cmp;
        config = toLuaFile ../config/nvim/nvim-cmp.lua;
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
        config = toLuaFile ../config/nvim/telescope.lua;
      }
      telescope-fzf-native-nvim
      telescope-github-nvim
      telescope-live-grep-args-nvim
      luasnip
      lualine-nvim
      nvim-web-devicons
      {
        plugin = nvim-tree-lua;
        config = toLuaFile ../config/nvim/nvim-tree.lua;
      }
      {
        plugin = null-ls-nvim;
        config = toLuaFile ../config/nvim/null-ls.lua;
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
        config = toLuaFile ../config/nvim/vim-test.lua;
      }
      vim-fugitive
      vim-rhubarb
      quickfix-reflector-vim
      vim-commentary
      vim-unimpaired
      {
        plugin = vim-tmux-navigator;
        config = toLuaFile ../config/nvim/vim-tmux-navigator.lua;
      }
      vim-tmux
    ];
  };
}
