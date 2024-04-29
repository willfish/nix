require('treesj').setup {}

vim.keymap.set('n', '<leader>j', require('treesj').toggle)
vim.keymap.set('n', '<leader>J', function()
    require('treesj').toggle({ split = { recursive = true } })
end)
