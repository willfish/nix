return {
  "nvimtools/none-ls.nvim", -- configure formatters & linters
  lazy = false,
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
        "black", -- python formatter
        "golangci_lint", -- golang linter
        "markdownlint", -- markdown linter
        "shellharden", -- shell formatter
        "shfmt", -- shell formatter
        "stylua", -- lua formatter
        "xmllint", -- xml formatter
      },
    })

    -- for conciseness
    local formatting = null_ls.builtins.formatting -- to setup formatters
    local diagnostics = null_ls.builtins.diagnostics -- to setup linters
    local codeactions = null_ls.builtins.code_actions -- to setup linters

    -- configure null_ls
    null_ls.setup({
      sources = {
        diagnostics.golangci_lint,
        diagnostics.markdownlint,
        formatting.black,
        formatting.hclfmt,
        formatting.markdownlint,
        formatting.shellharden,
        formatting.stylua,
        formatting.xmllint,
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
