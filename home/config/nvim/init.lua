vim.g.mapleader = ","
vim.g.maplocalleader = ","

vim.g.have_nerd_font = true

vim.opt.number = true
vim.opt.relativenumber = true
vim.opt.mouse = "a"
-- Don't show the mode, since it's already in the status line
vim.opt.showmode = false

vim.schedule(function()
	vim.opt.clipboard = "unnamedplus"
end)

vim.opt.breakindent = true
vim.opt.undofile = true
vim.opt.swapfile = false
vim.opt.inccommand = "split"
vim.opt.splitright = true
vim.opt.ignorecase = true
vim.opt.smartcase = true
vim.opt.signcolumn = "yes"
vim.opt.updatetime = 250
vim.opt.timeoutlen = 300
vim.opt.splitright = true
vim.opt.splitbelow = true
vim.opt.list = true
vim.opt.listchars = {
	tab = "→ ",
	trail = "·",
	nbsp = "␣",
	precedes = "«",
	extends = "»",
}
vim.opt.cursorline = true
vim.opt.scrolloff = 10

vim.keymap.set("n", "<Esc>", "<cmd>nohlsearch<CR>")
vim.keymap.set("t", "<Esc><Esc>", "<C-\\><C-n>", { desc = "Exit terminal mode" })
vim.keymap.set("i", "jk", "<Esc>", { desc = "Escape out of INSERT mode" })
vim.keymap.set("i", "kj", "<Esc>", { desc = "Escape out of INSERT mode" })
vim.keymap.set("n", "<Leader>w", ":w!<CR>", { desc = "[W]rite the current file" })
vim.keymap.set("n", "<Leader>q", ":wq!<CR>", { desc = "Write and [q]uit the current file" })

function insert_jira_ticket_number()
	local command = "git branch --show-current | sed -E 's/((HMRC|PRDEX|OTTIMP)-[0-9]+)-(.+)/\\1: /'"
	local handle = io.popen(command)
	local result = handle:read("*a")

	handle:close()

	result = result:gsub("\n$", "")

	vim.api.nvim_put({ result }, "c", true, true)
end

function insert_jira_ticket_url()
	local command = "git branch --show-current | sed -E 's/((HMRC|PRDEX|OTTIMP)-[0-9]+)-(.+)/\\1: /'"
	local handle = io.popen(command)
	local result = handle:read("*a")

	handle:close()

	result = result:gsub("\n$", "")
	result = result:gsub(": $", "")
	result = "[" .. result .. "]" .. "(https://transformuk.atlassian.net/browse/" .. result .. ")"

	vim.api.nvim_put({ result }, "c", true, true)
end

function toggle_quickfix()
	local quickfix_open = false

	for _, win in pairs(vim.fn.getwininfo()) do
		if win.quickfix == 1 then
			quickfix_open = true
			break
		end
	end

	if quickfix_open then
		vim.api.nvim_command("cclose")
	else
		vim.api.nvim_command("copen")
	end
end

vim.api.nvim_set_keymap("n", "<Leader>.", "<cmd>lua toggle_quickfix()<CR>", { desc = "Toggles the quickfix menu." })
vim.api.nvim_set_keymap(
	"n",
	"<leader>p",
	"<cmd>lua insert_jira_ticket_number()<CR>",
	{ desc = "Inserts the current branch ticket number into the buffer" }
)
vim.api.nvim_set_keymap(
	"n",
	"<leader>u",
	"<cmd>lua insert_jira_ticket_url()<CR>",
	{ desc = "Inserts markdown URL for the current jira ticket" }
)
vim.api.nvim_create_autocmd("TextYankPost", {
	desc = "Highlight when yanking (copying) text",
	group = vim.api.nvim_create_augroup("kickstart-highlight-yank", { clear = true }),
	callback = function()
		vim.highlight.on_yank()
	end,
})

local lazypath = "/home/william/.config/lazy"
if not (vim.uv or vim.loop).fs_stat(lazypath) then
	local lazyrepo = "https://github.com/folke/lazy.nvim.git"
	local out = vim.fn.system({ "git", "clone", "--filter=blob:none", "--branch=stable", lazyrepo, lazypath })
	if vim.v.shell_error ~= 0 then
		error("Error cloning lazy.nvim:\n" .. out)
	end
end ---@diagnostic disable-next-line: undefined-field
vim.opt.rtp:prepend(lazypath)

require("lazy").setup({
	"tpope/vim-sleuth", -- Detect tabstop and shiftwidth automatically
	"andymass/vim-matchup",
	"christoomey/vim-sort-motion",
	"fatih/vim-go",
	"lepture/vim-jinja",
	"stefandtw/quickfix-reflector.vim",
	"tpope/vim-bundler",
	"tpope/vim-dispatch",
	"tpope/vim-rhubarb",
	"tpope/vim-unimpaired",
	"xiyaowong/nvim-cursorword",
	{ "b4winckler/vim-angry", dependencies = "kana/vim-textobj-user" },
	{ "bps/vim-textobj-python", dependencies = "kana/vim-textobj-user" },
	{ "kana/vim-textobj-line", dependencies = "kana/vim-textobj-user" },
	{ "michaeljsmith/vim-indent-object", dependencies = "kana/vim-textobj-user" },
	{ "tek/vim-textobj-ruby", dependencies = "kana/vim-textobj-user" },
	{ "vimtaku/vim-textobj-keyvalue", dependencies = "kana/vim-textobj-user" },
	{ "whatyouhide/vim-textobj-erb", dependencies = "kana/vim-textobj-user" },
	{ "whatyouhide/vim-textobj-xmlattr", dependencies = "kana/vim-textobj-user" },
	{ "sotte/presenting.nvim", cmd = { "Presenting" } },
	{
		"kylechui/nvim-surround",
		version = "*",
		event = "VeryLazy",
		config = function()
			require("nvim-surround").setup({})
		end,
	},
	{
		"lewis6991/gitsigns.nvim",
		opts = {
			on_attach = function(bufnr)
				local gitsigns = require("gitsigns")
				local function map(mode, l, r, opts)
					opts = opts or {}
					opts.buffer = bufnr
					vim.keymap.set(mode, l, r, opts)
				end

				-- Navigation
				map("n", "]c", function()
					if vim.wo.diff then
						vim.cmd.normal({ "]c", bang = true })
					else
						gitsigns.nav_hunk("next")
					end
				end, { desc = "Jump to next git [c]hange" })

				map("n", "[c", function()
					if vim.wo.diff then
						vim.cmd.normal({ "[c", bang = true })
					else
						gitsigns.nav_hunk("prev")
					end
				end, { desc = "Jump to previous git [c]hange" })
			end,
		},
	},
	{
		"vim-test/vim-test",
		config = function()
			local default_map_opts = { noremap = true, silent = true }

			vim.api.nvim_set_var("test#strategy", {
				nearest = "basic",
				file = "dispatch",
				suite = "dispatch_background",
			})

			vim.api.nvim_set_keymap("n", "<Leader>x", ":TestNearest<CR>", default_map_opts)
			vim.api.nvim_set_keymap("n", "<Leader>t", ":TestFile<CR>", default_map_opts)
			vim.api.nvim_set_keymap("n", "<Leader>r", ":TestSuite<CR>", default_map_opts)
			vim.api.nvim_set_keymap("n", "<Leader>e", ":TestLast<CR>", default_map_opts)
			vim.api.nvim_set_keymap("n", "<Leader>l", ":TestVisit<CR>", default_map_opts)
		end,
	},
	{
		"NvChad/nvim-colorizer.lua",
		event = { "BufReadPre", "BufNewFile" },
		config = true,
	},
	{
		"Wansmer/treesj",
		config = function()
			require("treesj").setup({})

			vim.keymap.set("n", "<leader>j", require("treesj").toggle)
		end,
	},
	{
		"nvim-tree/nvim-tree.lua",
		dependencies = { "nvim-tree/nvim-web-devicons" },
		config = function()
			local nvimtree = require("nvim-tree")
			vim.g.loaded_netrw = 1
			vim.g.loaded_netrwPlugin = 1
			vim.cmd([[ highlight NvimTreeFolderArrowClosed guifg=#3FC5FF ]])
			vim.cmd([[ highlight NvimTreeFolderArrowOpen guifg=#3FC5FF ]])
			nvimtree.setup({
				filters = {
					custom = { ".DS_Store" },
				},
				git = {
					ignore = false,
				},
			})

			vim.keymap.set("n", "<leader>n", "<cmd>NvimTreeToggle<CR>", { desc = "Toggle file explorer" })
			vim.keymap.set(
				"n",
				"<leader>nf",
				"<cmd>NvimTreeFindFileToggle<CR>",
				{ desc = "Toggle file explorer on current file" }
			)
		end,
	},
	{
		"christoomey/vim-tmux-navigator",
		config = function()
			local default_map_opts = { noremap = true, silent = true }

			vim.g.tmux_navigator_no_mappings = 1

			vim.api.nvim_set_keymap("n", "<M-h>", ":TmuxNavigateLeft<CR>", default_map_opts)
			vim.api.nvim_set_keymap("n", "<M-j>", ":TmuxNavigateDown<CR>", default_map_opts)
			vim.api.nvim_set_keymap("n", "<M-k>", ":TmuxNavigateUp<CR>", default_map_opts)
			vim.api.nvim_set_keymap("n", "<M-l>", ":TmuxNavigateRight<CR>", default_map_opts)
		end,
	},
	{
		"tpope/vim-fugitive",
		dependencies = { "tpope/vim-rhubarb" },
		config = function()
			vim.keymap.set("n", "<Leader>i", ":Git ", { desc = "Enter a GIT command" })
			vim.keymap.set("n", "<Leader>b", ":Git blame<CR>", { desc = "Enter a GIT blame for the current buffer" })
			vim.keymap.set("n", "<Leader>o", ":GBrowse<CR>", { desc = "Open the current buffer in a browser" })
			vim.keymap.set("n", "<Leader>s", ":Git<CR>", { desc = "Switch to git status view" })
			vim.keymap.set("n", "<Leader>c", ":Git commit<CR>", { desc = "Create a commit with what's staged" })
			vim.keymap.set("n", "<Leader>]", ":Git push<CR>", { desc = "Push current commits to the remote" })
			vim.keymap.set("n", "<Leader>[", ":Git pull<CR>", { desc = "Pull latest commits from the remote" })

			vim.api.nvim_create_user_command("Browse", function(opts)
				vim.fn.system({ "xdg-open", opts.fargs[1] })
			end, { nargs = 1 })
		end,
	},
	{
		"ggandor/leap.nvim",
		dependencies = { "tpope/vim-repeat", "ggandor/flit.nvim" },
		config = function()
			require("flit").setup({
				keys = { f = "f", F = "F", t = "t", T = "T" },
				labeled_modes = "v",
				multiline = true,
				opts = {},
			})
		end,
	},
	{
		"nvim-telescope/telescope.nvim",
		event = "VimEnter",
		branch = "0.1.x",
		dependencies = {
			"nvim-lua/plenary.nvim",
			"nvim-telescope/telescope-github.nvim",
			"nvim-telescope/telescope-live-grep-args.nvim",
			{ "nvim-telescope/telescope-fzf-native.nvim", build = "make" },
			{ "nvim-telescope/telescope-ui-select.nvim" },
			{ "nvim-tree/nvim-web-devicons" },
		},
		config = function()
			require("telescope").setup({
				extensions = {
					["ui-select"] = {
						require("telescope.themes").get_dropdown(),
					},
					fzf = {
						fuzzy = true,
						override_generic_sorter = true,
						override_file_sorter = true,
						case_mode = "smart_case",
					},
				},
			})

			pcall(require("telescope").load_extension, "fzf")
			pcall(require("telescope").load_extension, "gh")
			pcall(require("telescope").load_extension, "live_grep_args")
			pcall(require("telescope").load_extension, "ui-select")

			local builtin = require("telescope.builtin")
			local rg_args = {
				find_command = {
					"rg",
					"--files",
					"--hidden",
					"--follow",
					"--glob",
					"!.git",
					"--glob",
					"!.svn",
					"--glob",
					"!.hg",
					"--glob",
					"!.bzr",
					"--glob",
					"!.tmp",
					"--glob",
					"!.DS_Store",
					"--glob",
					"!.gitignore",
					"--glob",
					"!.gitmodules",
					"--glob",
					"!.gitattributes",
					"--glob",
					"!.gitkeep",
					"--glob",
					"!.gitconfig",
					"--glob",
					"!temp_dirs",
				},
			}

			vim.keymap.set("n", "<C-f>", function()
				builtin.find_files(rg_args)
			end, { desc = "Search Files" })

			vim.keymap.set("n", "<C-g>", function()
				require("telescope").extensions.live_grep_args.live_grep_args(rg_args)
			end, { desc = "Search by Grep" })
			vim.keymap.set(
				"n",
				"<Leader>fp<CR>",
				":lua require('telescope').extensions.gh.pull_request()<CR>",
				{ desc = "[S]earch [P]ull requests" }
			)
			vim.keymap.set("n", "<leader>fh", builtin.help_tags, { desc = "[F]ind [H]elp" })
			vim.keymap.set("n", "<leader>fk", builtin.keymaps, { desc = "[F]ind [K]eymaps" })
			vim.keymap.set("n", "<leader>ff", builtin.find_files, { desc = "[F]ind [F]iles" })
			vim.keymap.set("n", "<leader>fs", builtin.builtin, { desc = "[F]ind [S]elect Telescope" })
			vim.keymap.set("n", "<leader>fw", builtin.grep_string, { desc = "[F]ind current [W]ord" })
			vim.keymap.set("n", "<leader>fg", builtin.live_grep, { desc = "[F]ind by [G]rep" })
			vim.keymap.set("n", "<leader>fd", builtin.diagnostics, { desc = "[F]ind [D]iagnostics" })
			vim.keymap.set("n", "<leader>fr", builtin.resume, { desc = "[F]ind [R]esume" })
			vim.keymap.set("n", "<leader>f.", builtin.oldfiles, { desc = '[F]ind Recent Files ("." for repeat)' })
			vim.keymap.set("n", "  ", builtin.buffers, { desc = "[ ] Find existing buffers" })
			vim.keymap.set("n", "<leader>/", function()
				builtin.current_buffer_fuzzy_find(require("telescope.themes").get_dropdown({
					winblend = 10,
					previewer = false,
				}))
			end, { desc = "[/] Fuzzily search in current buffer" })

			vim.keymap.set("n", "<leader>f/", function()
				builtin.live_grep({
					grep_open_files = true,
					prompt_title = "Live Grep in Open Files",
				})
			end, { desc = "[F]ind [/] in Open Files" })

			vim.keymap.set("n", "<leader>fn", function()
				builtin.find_files({ cwd = "~/.dotfiles" })
			end, { desc = "[F]ind [N]ix files" })
		end,
	},
	{
		"folke/lazydev.nvim",
		ft = "lua",
		opts = {
			library = {
				{ path = "${3rd}/luv/library", words = { "vim%.uv" } },
			},
		},
	},
	{
		"neovim/nvim-lspconfig",
		event = { "BufReadPre", "BufNewFile" },
		dependencies = {
			"hrsh7th/cmp-nvim-lsp",
			{ "antosha417/nvim-lsp-file-operations", config = true },
			{ "j-hui/fidget.nvim", opts = {} },
			"hrsh7th/cmp-nvim-lsp",
		},
		config = function()
			vim.api.nvim_create_autocmd("LspAttach", {
				group = vim.api.nvim_create_augroup("kickstart-lsp-attach", { clear = true }),
				callback = function(event)
					local map = function(keys, func, desc, mode)
						mode = mode or "n"
						vim.keymap.set(mode, keys, func, { buffer = event.buf, desc = "LSP: " .. desc })
					end
					map("gd", require("telescope.builtin").lsp_definitions, "[G]oto [D]efinition")
					map("gD", vim.lsp.buf.declaration, "[G]oto [D]eclaration")
					map("gr", require("telescope.builtin").lsp_references, "[G]oto [R]eferences")
					map("gI", require("telescope.builtin").lsp_implementations, "[G]oto [I]mplementation")
					map("ga", vim.lsp.buf.code_action, "[G]o [a]ction", { "n", "x" })
					map("gn", vim.lsp.buf.rename, "[G]o re[n]ame")
					map("<leader>D", require("telescope.builtin").lsp_type_definitions, "Type [D]efinition")
					map("<leader>ds", require("telescope.builtin").lsp_document_symbols, "[D]ocument [S]ymbols")
					map(
						"<leader>ws",
						require("telescope.builtin").lsp_dynamic_workspace_symbols,
						"[W]orkspace [S]ymbols"
					)

					local function client_supports_method(client, method, bufnr)
						if vim.fn.has("nvim-0.11") == 1 then
							return client:supports_method(method, bufnr)
						else
							return client.supports_method(method, { bufnr = bufnr })
						end
					end

					local client = vim.lsp.get_client_by_id(event.data.client_id)
					if
						client
						and client_supports_method(
							client,
							vim.lsp.protocol.Methods.textDocument_documentHighlight,
							event.buf
						)
					then
						local highlight_augroup =
							vim.api.nvim_create_augroup("kickstart-lsp-highlight", { clear = false })
						vim.api.nvim_create_autocmd({ "CursorHold", "CursorHoldI" }, {
							buffer = event.buf,
							group = highlight_augroup,
							callback = vim.lsp.buf.document_highlight,
						})

						vim.api.nvim_create_autocmd({ "CursorMoved", "CursorMovedI" }, {
							buffer = event.buf,
							group = highlight_augroup,
							callback = vim.lsp.buf.clear_references,
						})

						vim.api.nvim_create_autocmd("LspDetach", {
							group = vim.api.nvim_create_augroup("kickstart-lsp-detach", { clear = true }),
							callback = function(event2)
								vim.lsp.buf.clear_references()
								vim.api.nvim_clear_autocmds({ group = "kickstart-lsp-highlight", buffer = event2.buf })
							end,
						})
					end
					if
						client
						and client_supports_method(client, vim.lsp.protocol.Methods.textDocument_inlayHint, event.buf)
					then
						map("<leader>th", function()
							vim.lsp.inlay_hint.enable(not vim.lsp.inlay_hint.is_enabled({ bufnr = event.buf }))
						end, "[T]oggle Inlay [H]ints")
					end
				end,
			})
			vim.diagnostic.config({
				severity_sort = true,
				float = { border = "rounded", source = "if_many" },
				underline = { severity = vim.diagnostic.severity.ERROR },
				signs = vim.g.have_nerd_font and {
					text = {
						[vim.diagnostic.severity.ERROR] = "󰅚 ",
						[vim.diagnostic.severity.WARN] = "󰀪 ",
						[vim.diagnostic.severity.INFO] = "󰋽 ",
						[vim.diagnostic.severity.HINT] = "󰌶 ",
					},
				} or {},
				virtual_text = {
					source = "if_many",
					spacing = 2,
					format = function(diagnostic)
						local diagnostic_message = {
							[vim.diagnostic.severity.ERROR] = diagnostic.message,
							[vim.diagnostic.severity.WARN] = diagnostic.message,
							[vim.diagnostic.severity.INFO] = diagnostic.message,
							[vim.diagnostic.severity.HINT] = diagnostic.message,
						}
						return diagnostic_message[diagnostic.severity]
					end,
				},
			})

			local capabilities = vim.lsp.protocol.make_client_capabilities()
			capabilities = vim.tbl_deep_extend("force", capabilities, require("cmp_nvim_lsp").default_capabilities())

			vim.lsp.config("bashls", { capabilities = capabilities })
			vim.lsp.enable("bashls")
			vim.lsp.config("ccls", { capabilities = capabilities })
			vim.lsp.enable("ccls")
			vim.lsp.config("cssls", { capabilities = capabilities })
			vim.lsp.enable("cssls")
			vim.lsp.config("eslint", { capabilities = capabilities })
			vim.lsp.enable("eslint")
			vim.lsp.config("gopls", { capabilities = capabilities })
			vim.lsp.enable("gopls")
			vim.lsp.config("html", { capabilities = capabilities })
			vim.lsp.enable("html")
			vim.lsp.config("marksman", { capabilities = capabilities })
			vim.lsp.enable("marksman")
			vim.lsp.config("nil_ls", { capabilities = capabilities })
			vim.lsp.enable("nil_ls")
			vim.lsp.config("ols", { capabilities = capabilities })
			vim.lsp.enable("ols")
			vim.lsp.config("pyright", { capabilities = capabilities })
			vim.lsp.enable("pyright")
			vim.lsp.config("ruby_lsp", { capabilities = capabilities })
			vim.lsp.enable("ruby_lsp")
			vim.lsp.config("terraformls", { capabilities = capabilities })
			vim.lsp.enable("terraformls")
			vim.lsp.config("ts_ls", { capabilities = capabilities })
			vim.lsp.enable("ts_ls")

			vim.lsp.config("lua_ls", {
				capabilities = capabilities,
				settings = {
					Lua = {
						diagnostics = {
							globals = { "vim" },
						},
						workspace = {
							library = {
								[vim.fn.expand("$VIMRUNTIME/lua")] = true,
								[vim.fn.stdpath("config") .. "/lua"] = true,
							},
						},
					},
				},
			})
			vim.lsp.enable("lua_ls")
		end,
	},
	{
		"williamboman/mason.nvim",
		dependencies = {
			"williamboman/mason-lspconfig.nvim",
			"WhoIsSethDaniel/mason-tool-installer.nvim",
		},
		config = function()
			local mason = require("mason")
			local mason_lspconfig = require("mason-lspconfig")
			local mason_tool_installer = require("mason-tool-installer")

			mason.setup({
				ui = {
					icons = {
						package_installed = "✓",
						package_pending = "➜",
						package_uninstalled = "✗",
					},
				},
			})

			mason_lspconfig.setup({
				ensure_installed = { "html", "pyright", "terraformls" },
				automatic_installation = true,
			})

			mason_tool_installer.setup({ ensure_installed = { "black", "isort" } })
		end,
	},
	{
		"stevearc/conform.nvim",
		event = { "BufWritePre" },
		cmd = { "ConformInfo" },
		keys = {
			{
				"<leader>a",
				function()
					require("conform").format({ async = true, lsp_format = "fallback" })
				end,
				mode = "",
			},
		},
		opts = {
			notify_on_error = false,
			format_on_save = function(bufnr)
				local disable_filetypes = { c = true, cpp = true }
				local lsp_format_opt
				if disable_filetypes[vim.bo[bufnr].filetype] then
					lsp_format_opt = "never"
				else
					lsp_format_opt = "fallback"
				end
				return {
					timeout_ms = 500,
					lsp_format = lsp_format_opt,
				}
			end,
			formatters_by_ft = {
				fish = { "fish_indent" },
				javascript = { "prettier", stop_after_first = true },
				lua = { "stylua" },
				python = { "isort", "black" },
			},
		},
	},

	{
		"hrsh7th/nvim-cmp",
		event = "InsertEnter",
		dependencies = {
			{
				"L3MON4D3/LuaSnip",
				build = (function()
					if vim.fn.has("win32") == 1 or vim.fn.executable("make") == 0 then
						return
					end
					return "make install_jsregexp"
				end)(),
				dependencies = {
					{
						"rafamadriz/friendly-snippets",
						config = function()
							require("luasnip.loaders.from_vscode").lazy_load()
						end,
					},
				},
			},
			"saadparwaiz1/cmp_luasnip",
			"hrsh7th/cmp-nvim-lsp",
			"hrsh7th/cmp-path",
			"hrsh7th/cmp-nvim-lsp-signature-help",
			"github/copilot.vim",
		},
		config = function()
			local cmp = require("cmp")
			local luasnip = require("luasnip")
			luasnip.config.setup({})

			cmp.setup({
				snippet = {
					expand = function(args)
						luasnip.lsp_expand(args.body)
					end,
				},
				completion = { completeopt = "menu,menuone,noinsert" },
				mapping = cmp.mapping.preset.insert({
					["<C-n>"] = cmp.mapping.select_next_item(),
					["<C-p>"] = cmp.mapping.select_prev_item(),
					["<C-b>"] = cmp.mapping.scroll_docs(-4),
					["<C-f>"] = cmp.mapping.scroll_docs(4),
					["<C-y>"] = cmp.mapping.confirm({ select = true }),
					["<C-Space>"] = cmp.mapping.complete({}),
					["<C-l>"] = cmp.mapping(function()
						if luasnip.expand_or_locally_jumpable() then
							luasnip.expand_or_jump()
						end
					end, { "i", "s" }),
					["<C-h>"] = cmp.mapping(function()
						if luasnip.locally_jumpable(-1) then
							luasnip.jump(-1)
						end
					end, { "i", "s" }),
				}),
				sources = {
					{ name = "lazydev", group_index = 0 },
					{ name = "nvim_lsp" },
					{ name = "luasnip" },
					{ name = "path" },
					{ name = "nvim_lsp_signature_help" },
					{ name = "copilot" },
				},
			})
		end,
	},
	{
		"folke/tokyonight.nvim",
		priority = 1000,
		config = function()
			require("tokyonight").setup({ styles = { comments = { italic = false } } })
			vim.cmd.colorscheme("tokyonight-night")
		end,
	},
	{
		"folke/todo-comments.nvim",
		event = "VimEnter",
		dependencies = { "nvim-lua/plenary.nvim" },
		opts = { signs = false },
	},

	{
		"echasnovski/mini.nvim",
		config = function()
			local statusline = require("mini.statusline")
			statusline.setup({ use_icons = vim.g.have_nerd_font })
			statusline.section_location = function()
				return "%2l:%-2v"
			end
		end,
	},
	{
		"nvim-treesitter/nvim-treesitter",
		build = ":TSUpdate",
		main = "nvim-treesitter.configs",
		opts = {
			ensure_installed = {
				"bash",
				"c",
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
				"haskell",
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
				"python",
				"query",
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
				"vimdoc",
				"yaml",
			},
			auto_install = true,
			highlight = {
				enable = true,
				additional_vim_regex_highlighting = { "ruby" },
			},
			indent = { enable = true, disable = { "ruby" } },
		},
	},
}, { ui = { icons = vim.g.have_nerd_font } })
