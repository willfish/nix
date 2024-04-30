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

    vim.api.nvim_create_autocmd(
        "VimEnter",
        {
            pattern = "*",
            nested = true,
            callback = function()
                vim.schedule(
                    function()
                        local os = require("os")
                        local default_map_opts = { noremap = true, silent = true }
                        local rg_args =
                        "{find_command = {'rg', '--files', '--hidden', '--follow', '--glob', '!.git', '--glob', '!.svn', '--glob', '!.hg', '--glob', '!.bzr', '--glob', '!.tmp', '--glob', '!.DS_Store', '--glob', '!.gitignore', '--glob', '!.gitmodules', '--glob', '!.gitattributes', '--glob', '!.gitkeep', '--glob', '!.gitconfig', '--glob', '!temp_dirs'}}"
                        if (os.execute("test -e .git/") == 0) then
                            local is_cypress_project = os.execute("test -e cypress/") == 0
                            -- if cypress project or in home directory
                            if (is_cypress_project or vim.fn.expand("%:p:h") == os.getenv("HOME")) then
                                rg_args = rg_args .. ", {previewer = false}"
                            end
                            vim.api.nvim_set_keymap(
                                "n",
                                "<C-f>",
                                string.format(":lua require('telescope.builtin').git_files(%s)<CR>", rg_args),
                                default_map_opts
                            )
                        else
                            vim.api.nvim_set_keymap(
                                "n",
                                "<C-f>",
                                string.format(":lua require('telescope.builtin').find_files(%s)<CR>", rg_args),
                                default_map_opts
                            )
                        end
                    end
                )
            end,
            group = style_group_id
        }
    )
end

M.create_augroups_and_aucmds()

return M
