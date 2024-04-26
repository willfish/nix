local default_map_opts = {noremap = true, silent = true}

vim.g.tmux_navigator_no_mappings = 1

vim.api.nvim_set_keymap("n", "<M-h>", ":TmuxNavigateLeft<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<M-j>", ":TmuxNavigateDown<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<M-k>", ":TmuxNavigateUp<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<M-l>", ":TmuxNavigateRight<CR>", default_map_opts)

