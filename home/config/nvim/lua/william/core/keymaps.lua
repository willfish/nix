local default_opts = { noremap = true, silent = true }

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

vim.api.nvim_set_keymap("n", "<Leader>wn", ":lua os.execute('/usr/bin/variety -n > /dev/null 2>&1')<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>wp", ":lua os.execute('/usr/bin/variety -p > /dev/null 2>&1')<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>m", ":Make<CR>", default_opts)

-- Plugin maps
vim.api.nvim_set_keymap("n", "<Leader>i", ":Git ", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>b", ":Git blame<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>o", ":GBrowse<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>s", ":Git<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>c", ":Git commit<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>]", ":Git push<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<Leader>[", ":Git pull<CR>", default_opts)

vim.api.nvim_set_keymap("n", "<leader>n", ":NvimTreeToggle<CR>", default_opts)
vim.api.nvim_set_keymap("n", "<leader>nf", ":NvimTreeFindFile<CR>", default_opts)

function toggle_quickfix()
    local quickfix_open = false
    -- Check all windows to see if the quickfix window is open
    for _, win in pairs(vim.fn.getwininfo()) do
        if win.quickfix == 1 then
            quickfix_open = true
            break
        end
    end

    if quickfix_open then
        -- Use API function to close the quickfix window safely
        vim.api.nvim_command('cclose')
    else
        -- Use API function to open the quickfix window safely
        vim.api.nvim_command('copen')
    end
end

function insert_jira_ticket_number()
    local command = "git branch --show-current | sed -E 's/((OTT|FPO|BAU|GL|PTE)-[0-9]+)-(.+)/\\1: /'"
    local handle = io.popen(command)
    local result = handle:read("*a")

    handle:close()

    result = result:gsub("\n$", "")

    vim.api.nvim_put({ result }, 'c', true, true)
end

vim.api.nvim_set_keymap("n", "<Leader>.", "<cmd>lua toggle_quickfix()<CR>", default_opts)
vim.api.nvim_set_keymap('n', '<leader>p', '<cmd>lua insert_jira_ticket_number()<CR>', default_opts)

vim.g.loaded = 1
vim.g.loaded_netrwPlugin = 1
