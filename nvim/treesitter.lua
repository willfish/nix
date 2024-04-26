require("nvim-treesitter.configs").setup {
    ensure_installed = "all",
    highlight = {
        enable = true
    },
    incremental_selection = {
        enable = true,
        keymaps = {
            init_selection = "<CR>",
            node_incremental = "<CR>",
            scope_incremental = "<Tab>",
            node_decremental = "<S-CR>"
        }
    },
    textsubjects = {
        enable = true,
        keymaps = {
            ["."] = "textsubjects-smart"
        }
    },
    endwise = {
        enable = true
    },
    matchup = {
        enable = true
    }
}
