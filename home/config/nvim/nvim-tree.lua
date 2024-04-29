local default_map_opts = {noremap = true, silent = true}

require("nvim-tree").setup(
    {
        sort_by = "case_sensitive",
        renderer = {
            group_empty = true
        },
        filters = {
            dotfiles = true
        }
    }
)

vim.api.nvim_set_keymap("n", "<leader>n", ":NvimTreeToggle<CR>", default_map_opts)
vim.api.nvim_set_keymap("n", "<leader>nf", ":NvimTreeFindFile<CR>", default_map_opts)

vim.g.loaded = 1
vim.g.loaded_netrwPlugin = 1
