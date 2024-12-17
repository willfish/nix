return {
	{
		"nvim-treesitter/nvim-treesitter",
		event = { "BufReadPre", "BufNewFile" },
		build = ":TSUpdate",
		dependencies = {
			"nvim-treesitter/nvim-treesitter-textobjects",
			"windwp/nvim-ts-autotag",
		},
		config = function()
			-- import nvim-treesitter plugin
			local treesitter = require("nvim-treesitter.configs")

			-- configure treesitter
			treesitter.setup({ -- enable syntax highlighting
				highlight = {
					enable = true,
				},
				-- enable indentation
				indent = { enable = true },
				-- enable autotagging (w/ nvim-ts-autotag plugin)
				autotag = {
					enable = true,
				},
				-- ensure these language parsers are installed
				ensure_installed = {
					"bash",
					"css",
					"diff",
					"dockerfile",
					"editorconfig",
					"fish",
					"git_config",
					"git_rebase",
					"gitattributes",
					"gitcommit",
					"gitignore",
					"go",
					"goctl",
					"gosum",
					"graphql",
					"html",
					"htmldjango",
					"javascript",
					"json",
					"lua",
					"luadoc",
					"make",
					"markdown",
					"markdown_inline",
					"mermaid",
					"nginx",
					"ninja",
					"nix",
					"passwd",
					"robots",
					"ruby",
					"rust",
					"sql",
					"ssh_config",
					"tmux",
					"toml",
					"tsx",
					"typescript",
					"vim",
					"yaml",
				},
				incremental_selection = {
					enable = true,
					keymaps = {
						init_selection = "<C-space>",
						node_incremental = "<C-space>",
						scope_incremental = false,
						node_decremental = "<bs>",
					},
				},
				enable_autocmd = false,
			})
		end,
	},
}
