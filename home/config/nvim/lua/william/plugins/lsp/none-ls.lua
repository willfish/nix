return {
  "nvimtools/none-ls.nvim", -- configure formatters & linters
  lazy = true,
  -- event = { "BufReadPre", "BufNewFile" }, -- to enable uncomment this
  dependencies = {
    "jay-babu/mason-null-ls.nvim",
  },
  config = function()
    local mason_null_ls = require("mason-null-ls")

    local null_ls = require("null-ls")

    local null_ls_utils = require("null-ls.utils")

    mason_null_ls.setup({
      ensure_installed = {
        "prettier", -- prettier formatter
        "stylua", -- lua formatter
        "black", -- python formatter
        "pylint", -- python linter
        "eslint-lsp", -- js linter
      },
    })

    -- for conciseness
    local formatting = null_ls.builtins.formatting -- to setup formatters
    local diagnostics = null_ls.builtins.diagnostics -- to setup linters

    -- configure null_ls
    null_ls.setup({
      -- add package.json as identifier for root (for typescript monorepos)
      root_dir = null_ls_utils.root_pattern(".null-ls-root", "Makefile", ".git", "package.json"),
      -- setup formatters & linters
      sources = {
        diagnostics.erb_lint,
        diagnostics.eslint,
        diagnostics.fish,
        diagnostics.flake8,
        diagnostics.golangci_lint,
        diagnostics.markdownlint,
        formatting.autopep8,
        formatting.black,
        formatting.erb_lint,
        formatting.fish_indent,
        formatting.hclfmt,
        formatting.isort,
        formatting.markdownlint,
        formatting.shellharden,
        formatting.sql_formatter,
        formatting.stylua, -- lua formatter
        formatting.trim_whitespace,
        formatting.xmllint,

        -- diagnostics.eslint_d.with({ -- js/ts linter
        --   condition = function(utils)
        --     return utils.root_has_file({ ".eslintrc.js", ".eslintrc.cjs" }) -- only enable if root has .eslintrc.js or .eslintrc.cjs
        --   end,
        -- }),
      },
      -- configure format on save
      on_attach = function(current_client, bufnr)
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
    })
  end,
}
