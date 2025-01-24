return {
	'christoomey/vim-tmux-navigator', -- tmux & split window navigation
	'fedepujol/move.nvim',
	'inkarkat/vim-ReplaceWithRegister',
	'nvim-lua/plenary.nvim', -- lua functions that many plugins use
  'Glench/Vim-Jinja2-Syntax',
  'RRethy/nvim-treesitter-endwise',
  'RRethy/nvim-treesitter-textsubjects',
  'andymass/vim-matchup',
  'christoomey/vim-sort-motion',
  'fadein/vim-FIGlet',
  'fatih/vim-go',
  'junegunn/vim-easy-align',
  'kevinhwang91/nvim-bqf',
  'kylechui/nvim-surround',
  'lepture/vim-jinja',
  'sheerun/vim-polyglot',
  'stefandtw/quickfix-reflector.vim',
  'tpope/vim-bundler',
  'tpope/vim-dispatch',
  'tpope/vim-rhubarb',
  'tpope/vim-unimpaired',
  'vim-test/vim-test',
  'xiyaowong/nvim-cursorword',
  'xiyaowong/telescope-emoji.nvim',
  'xiyaowong/transparent.nvim',
  { 'b4winckler/vim-angry',            dependencies = 'kana/vim-textobj-user' },
  { 'bps/vim-textobj-python',          dependencies = 'kana/vim-textobj-user' },
  { 'kana/vim-textobj-line',           dependencies = 'kana/vim-textobj-user' },
  { 'michaeljsmith/vim-indent-object', dependencies = 'kana/vim-textobj-user' },
  { 'tek/vim-textobj-ruby',            dependencies = 'kana/vim-textobj-user' },
  { 'vimtaku/vim-textobj-keyvalue',    dependencies = 'kana/vim-textobj-user' },
  { 'whatyouhide/vim-textobj-erb',     dependencies = 'kana/vim-textobj-user' },
  { 'whatyouhide/vim-textobj-xmlattr', dependencies = 'kana/vim-textobj-user' },
  {
    "stevearc/conform.nvim",
    optional = true,
    opts = {
      formatters_by_ft = {
        nix = { "nixfmt" },
      },
    },
  }
}
