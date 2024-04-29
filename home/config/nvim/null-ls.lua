local null_ls = require("null-ls")

null_ls.setup({
    on_attach = function(_client, bufnr)
        -- fetch normal mode keymaps for the current buffer
        local norm_maps = vim.api.nvim_buf_get_keymap(bufnr, "n")

        -- filter down to only the keymap we're interested in
        norm_maps = vim.tbl_filter(function(v)
            return v.lhs == "<leader>a"
        end, norm_maps)

        if #norm_maps == 0 then
            vim.keymap.set(
                "n",
                "<leader>a",
                function()
                    vim.lsp.buf.format { async = true }
                end,
                { noremap = true, silent = true }
            )
        end
    end,
    sources = {
        null_ls.builtins.code_actions.proselint,
        null_ls.builtins.diagnostics.erb_lint,
        null_ls.builtins.diagnostics.eslint,
        null_ls.builtins.diagnostics.fish,
        null_ls.builtins.diagnostics.flake8,
        null_ls.builtins.diagnostics.golangci_lint,
        null_ls.builtins.diagnostics.markdownlint,
        null_ls.builtins.formatting.autopep8,
        null_ls.builtins.formatting.black,
        null_ls.builtins.formatting.erb_lint,
        null_ls.builtins.formatting.eslint,
        null_ls.builtins.formatting.fish_indent,
        null_ls.builtins.formatting.hclfmt,
        null_ls.builtins.formatting.markdownlint,
        null_ls.builtins.formatting.shellharden,
        null_ls.builtins.formatting.sql_formatter,
        null_ls.builtins.formatting.trim_whitespace,
        null_ls.builtins.formatting.xmllint,
    },
})
