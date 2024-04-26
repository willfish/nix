local default_map_opts = {noremap = true, silent = true}

vim.api.nvim_set_var(
    "test#strategy",
    {
        nearest = "basic",
        file = "dispatch",
        suite = "dispatch_background"
    }
)

vim.api.nvim_set_var("test#ruby#rspec#executable", "bundle exec rspec")
vim.api.nvim_set_var("test#python#pytest#executable", "pytest")

vim.api.nvim_set_keymap("n", "<Leader>x", ":TestNearest<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>t", ":TestFile<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>r", ":TestSuite<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>e", ":TestLast<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<Leader>l", ":TestVisit<CR>", default_map_opts)
