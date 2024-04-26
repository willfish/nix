local default_opts = { noremap = true, silent = true }

local init_vim = "~/.config/nvim/init.lua"
local init_lua = "~/.config/nvim/lua/willfish/init.lua"
local opts = "~/.config/nvim/lua/willfish/opts.lua"
local maps = "~/.config/nvim/lua/willfish/maps.lua"
local autocmds = "~/.config/nvim/lua/willfish/autocmds.lua"
local plugins = "~/.config/nvim/lua/willfish/plugins.lua"

local fish_config = "~/.config/fish/config.fish"
local i3_config = "~/.i3/config"
local tmux_config = "~/.tmux.conf"

-- xrandr --output DP-1-2 --auto --primary --rotate normal --output eDP-1 --preferred --rotate normal --below DP-2

-- Config file normal maps

vim.api.nvim_set_keymap("n", "<Leader>ef", ":edit" .. fish_config .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>ei", ":edit" .. i3_config .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>et", ":edit" .. tmux_config .. "<CR>", default_opts)

vim.api.nvim_set_keymap("n", "<Leader>ev", ":edit" .. init_vim .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>evi", ":edit" .. init_lua .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>evo", ":edit" .. opts .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>evm", ":edit" .. maps .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>evp", ":edit" .. plugins .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>evd", ":edit" .. autocmds .. "<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>sv", ":source" .. init_vim .. "<CR>", default_opts)

-- Awesome bindings -- primeagen
vim.api.nvim_set_keymap("n", "Y", "y$", default_opts)
vim.api.nvim_set_keymap("n", "n", "nzzzv", default_opts)
vim.api.nvim_set_keymap("n", "N", "Nzzzv", default_opts)
vim.api.nvim_set_keymap("n", "J", "mzJ`z", default_opts)
vim.api.nvim_set_keymap("i", ",", ",<c-g>u", default_opts)
vim.api.nvim_set_keymap("i", ".", ".<c-g>u", default_opts)
vim.api.nvim_set_keymap("i", "[", "[<c-g>u", default_opts)
vim.api.nvim_set_keymap("i", "!", "!<c-g>u", default_opts)
vim.api.nvim_set_keymap("i", "?", "?<c-g>u", default_opts)

-- Normal mode maps
vim.api.nvim_set_keymap("n", "Q", "<Nop>", default_opts)
vim.api.nvim_set_keymap("n", "0", "^", default_opts)
vim.api.nvim_set_keymap("n", "^", "0", default_opts)
vim.api.nvim_set_keymap("n", "<Space>", "za", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>w", ":w!<CR>", default_opts)

vim.api.nvim_set_keymap("n", "<Leader>q", ":wq!<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>v", ":vsplit<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader><Leader>", ":only<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>.", ":copen<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>`", "<c-w>=", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>/", ":!%:p<CR>", default_opts)

-- Insert mode maps
vim.api.nvim_set_keymap("i", "jk", "<Esc>", default_opts)
vim.api.nvim_set_keymap("i", "kj", "<Esc>", default_opts)
--

-- Terminal mode maps
vim.api.nvim_set_keymap("t", "<Esc>", "<C-\\><C-n>", default_opts)
vim.api.nvim_set_keymap("t", "jj", "<C-\\><C-n>", default_opts)
vim.api.nvim_set_keymap("t", "kj", "<C-\\><C-n>", default_opts)

-- Visual mode maps

vim.api.nvim_set_keymap("v", "J", ":m '>+1<CR>gv=gv", default_opts) -- Visually select and move lines up and down
vim.api.nvim_set_keymap("v", "K", ":m '>-2<CR>gv=gv", default_opts) -- Visually select and move lines up and down

-- Utilities maps
--
-- Multiple replace with s*
-- hit . to repeatedly replace a change to the word under the cursor
vim.api.nvim_set_keymap("n", "s*", ":let @/='\\<'.expand('<cword>').'\\>'<CR>cgn", default_opts) --
vim.api.nvim_set_keymap("x", "s*", '"sy:let @/=@s<CR>cgn', default_opts)

vim.api.nvim_set_keymap("x", "<leader>d", 'c<c-r>=system(\'base64 --decode\', @")<cr><esc>', default_opts)
vim.api.nvim_set_keymap("x", "<leader>e", 'c<c-r>=system(\'base64\', @")<cr><esc>', default_opts)

function insertJiraTicketNumber()
    local command = "git branch --show-current | sed -E 's/((HOTT|FPO|BAU)-[0-9]+)-(.+)/\\1: /'"
    local handle = io.popen(command)
    local result = handle:read("*a")

    handle:close()

    result = result:gsub("\n$", "")

    vim.api.nvim_put({ result }, 'c', true, true)
end

vim.api.nvim_set_keymap('n', '<leader>p', '<cmd>lua insertJiraTicketNumber()<CR>', default_opts)

vim.api.nvim_set_keymap("n", "<Leader>wn", ":lua os.execute('/usr/bin/variety -n > /dev/null 2>&1')<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>wp", ":lua os.execute('/usr/bin/variety -p > /dev/null 2>&1')<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>m", ":Make<CR>", default_opts)
