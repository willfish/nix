local default_map_opts = {noremap = true, silent = true}

vim.api.nvim_set_keymap("n", "<Leader>i", ":Git ", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>b", ":Git blame<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>o", ":GBrowse<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>s", ":Git<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>c", ":Git commit<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>]", ":Git push<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>[", ":Git pull<CR>", default_map_opts)
