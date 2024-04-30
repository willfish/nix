local M = {}

function M.create_augroups_and_aucmds()
    local style_group_id = vim.api.nvim_create_augroup("Styles", { clear = true })

    vim.api.nvim_create_autocmd(
        "FileType",
        {
            pattern = { "help", "qf", "fugitive" },
            callback = function()
                vim.schedule(
                    function()
                        vim.api.nvim_buf_set_keymap(0, "n", "q", ":q<CR>", { noremap = true, silent = true })
                    end
                )
            end,
            group = style_group_id
        }
    )

    vim.api.nvim_create_autocmd(
        "TextYankPost",
        {
            pattern = "*",
            callback = function()
                vim.schedule(
                    function()
                        require("vim.highlight").on_yank()
                    end
                )
            end,
            group = style_group_id
        }
    )
end

M.create_augroups_and_aucmds()

return M
