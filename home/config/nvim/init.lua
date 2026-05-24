vim.g.mapleader = ","
vim.g.maplocalleader = ","

vim.g.have_nerd_font = true

-- Disable vim-matchup's treesitter integration for stability on Neovim 0.12+.
-- The classic (non-treesitter) matching engine is still active and sufficient.
vim.g.matchup_treesitter_enabled = false

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

local function insert_jira_ticket_number()
	local result = vim.fn.system("git branch --show-current | sed -E 's/((HMRC|PRDEX|OTTIMP|AI)-[0-9]+)-(.+)/\\1: /'")
	result = result:gsub("\n$", "")
	vim.api.nvim_put({ result }, "c", true, true)
end

local function insert_jira_ticket_url()
	local ticket = vim.fn.system("git branch --show-current | sed -E 's/((HMRC|PRDEX|OTTIMP|AI)-[0-9]+)-.+/\\1/'")
	ticket = ticket:gsub("\n$", "")
	local result = "[" .. ticket .. "](https://transformuk.atlassian.net/browse/" .. ticket .. ")"
	vim.api.nvim_put({ result }, "c", true, true)
end

local function toggle_quickfix()
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

vim.keymap.set("n", "<Leader>.", toggle_quickfix, { desc = "Toggles the quickfix menu." })
vim.keymap.set(
	"n",
	"<Leader>p",
	insert_jira_ticket_number,
	{ desc = "Inserts the current branch ticket number into the buffer" }
)
vim.keymap.set("n", "<Leader>u", insert_jira_ticket_url, { desc = "Inserts markdown URL for the current jira ticket" })
vim.api.nvim_create_autocmd("TextYankPost", {
	desc = "Highlight when yanking (copying) text",
	group = vim.api.nvim_create_augroup("kickstart-highlight-yank", { clear = true }),
	callback = function()
		vim.highlight.on_yank()
	end,
})

vim.api.nvim_create_user_command("LspInfo", function()
	vim.print(vim.tbl_map(function(client)
		return {
			id = client.id,
			name = client.name,
			root = client.config.root_dir,
			cmd = client.config.cmd,
		}
	end, vim.lsp.get_clients({ bufnr = 0 })))
end, { desc = "Print LSP clients attached to the current buffer" })

local loaded_plugins = {}

local function packadd(plugin)
	if loaded_plugins[plugin] then
		return
	end
	vim.cmd.packadd(plugin)
	loaded_plugins[plugin] = true
end

local function packadd_many(plugins)
	for _, plugin in ipairs(plugins) do
		packadd(plugin)
	end
end

local function require_after_packadd(module, plugins)
	packadd_many(plugins)
	return require(module)
end

local function setup_presenting()
	local presenting = require_after_packadd("presenting", { "presenting.nvim" })
	presenting.setup({
		options = { width = 80 },
		configure_slide_buffer = function(buf)
			vim.bo[buf].filetype = "markdown"
			vim.wo[0].conceallevel = 2
			vim.wo[0].concealcursor = "nvic"
		end,
	})
end

vim.api.nvim_create_user_command("Presenting", function(opts)
	vim.api.nvim_del_user_command("Presenting")
	setup_presenting()
	vim.cmd({
		cmd = "Presenting",
		args = opts.args ~= "" and opts.args or nil,
		bang = opts.bang,
	})
end, {
	nargs = "*",
	bang = true,
	complete = "file",
})

vim.api.nvim_create_autocmd("User", {
	pattern = "DeferredUi",
	once = true,
	callback = function()
		require_after_packadd("nvim-surround", { "nvim-surround" }).setup({})

		require_after_packadd("noice", {
			"nui.nvim",
			"nvim-notify",
			"noice.nvim",
		}).setup({
			lsp = {
				override = {
					["vim.lsp.util.convert_input_to_markdown_lines"] = true,
					["vim.lsp.util.stylize_markdown"] = true,
				},
			},
			presets = {
				bottom_search = true,
				command_palette = true,
				long_message_to_split = true,
				inc_rename = false,
				lsp_doc_border = false,
			},
		})
	end,
})

vim.api.nvim_create_autocmd("UIEnter", {
	once = true,
	callback = function()
		vim.api.nvim_exec_autocmds("User", { pattern = "DeferredUi" })
	end,
})

require("gitsigns").setup({
	on_attach = function(bufnr)
		local gitsigns = require("gitsigns")
		local function map(mode, l, r, opts)
			opts = opts or {}
			opts.buffer = bufnr
			vim.keymap.set(mode, l, r, opts)
		end

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
})

vim.g["test#strategy"] = {
	nearest = "basic",
	file = "dispatch",
	suite = "dispatch_background",
}

vim.keymap.set("n", "<Leader>x", ":TestNearest<CR>", { silent = true, desc = "Test nearest" })
vim.keymap.set("n", "<Leader>t", ":TestFile<CR>", { silent = true, desc = "Test file" })
vim.keymap.set("n", "<Leader>r", ":TestSuite<CR>", { silent = true, desc = "Test suite" })
vim.keymap.set("n", "<Leader>e", ":TestLast<CR>", { silent = true, desc = "Test last" })
vim.keymap.set("n", "<Leader>l", ":TestVisit<CR>", { silent = true, desc = "Test visit" })

vim.api.nvim_create_autocmd({ "BufReadPre", "BufNewFile" }, {
	once = true,
	callback = function()
		require_after_packadd("colorizer", { "nvim-colorizer.lua" }).setup()
	end,
})

vim.keymap.set("n", "<leader>j", function()
	local treesj = require_after_packadd("treesj", { "treesj" })
	treesj.setup({})
	treesj.toggle()
end, { desc = "Split or join treesitter node" })

vim.g.loaded_netrw = 1
vim.g.loaded_netrwPlugin = 1

local function setup_nvim_tree()
	local nvimtree = require_after_packadd("nvim-tree", {
		"nvim-web-devicons",
		"nvim-tree.lua",
	})
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
end

vim.keymap.set("n", "<leader>n", function()
	setup_nvim_tree()
	vim.cmd.NvimTreeToggle()
end, { desc = "Toggle file explorer" })

vim.keymap.set("n", "<leader>nf", function()
	setup_nvim_tree()
	vim.cmd.NvimTreeFindFileToggle()
end, { desc = "Toggle file explorer on current file" })

vim.keymap.set("n", "<M-h>", "<cmd>TmuxNavigateLeft<CR>")
vim.keymap.set("n", "<M-j>", "<cmd>TmuxNavigateDown<CR>")
vim.keymap.set("n", "<M-k>", "<cmd>TmuxNavigateUp<CR>")
vim.keymap.set("n", "<M-l>", "<cmd>TmuxNavigateRight<CR>")

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

vim.keymap.set({ "n", "x", "o" }, "S", function()
	require_after_packadd("flash", { "flash.nvim" }).jump({ modes = { char = { enabled = true } } })
end, { desc = "Flash" })

local telescope_loaded = false
local function setup_telescope()
	if telescope_loaded then
		return
	end
	packadd_many({
		"plenary.nvim",
		"nvim-web-devicons",
		"telescope-fzf-native.nvim",
		"telescope-github.nvim",
		"telescope-live-grep-args.nvim",
		"telescope-ui-select.nvim",
		"telescope.nvim",
	})

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
	telescope_loaded = true
end

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

local function telescope_builtin(name, opts)
	return function()
		setup_telescope()
		require("telescope.builtin")[name](opts)
	end
end

vim.keymap.set("n", "<C-f>", telescope_builtin("find_files", rg_args), { desc = "Search Files" })
vim.keymap.set("n", "<C-g>", function()
	setup_telescope()
	require("telescope").extensions.live_grep_args.live_grep_args(rg_args)
end, { desc = "Search by Grep" })
vim.keymap.set("n", "<Leader>fp", function()
	setup_telescope()
	require("telescope").extensions.gh.pull_request()
end, { desc = "[F]ind [P]ull requests" })
vim.keymap.set("n", "<leader>fh", telescope_builtin("help_tags"), { desc = "[F]ind [H]elp" })
vim.keymap.set("n", "<leader>fk", telescope_builtin("keymaps"), { desc = "[F]ind [K]eymaps" })
vim.keymap.set("n", "<leader>ff", telescope_builtin("find_files"), { desc = "[F]ind [F]iles" })
vim.keymap.set("n", "<leader>fs", telescope_builtin("builtin"), { desc = "[F]ind [S]elect Telescope" })
vim.keymap.set("n", "<leader>fw", telescope_builtin("grep_string"), { desc = "[F]ind current [W]ord" })
vim.keymap.set("n", "<leader>fg", telescope_builtin("live_grep"), { desc = "[F]ind by [G]rep" })
vim.keymap.set("n", "<leader>fd", telescope_builtin("diagnostics"), { desc = "[F]ind [D]iagnostics" })
vim.keymap.set("n", "<leader>fr", telescope_builtin("resume"), { desc = "[F]ind [R]esume" })
vim.keymap.set("n", "<leader>f.", telescope_builtin("oldfiles"), { desc = '[F]ind Recent Files ("." for repeat)' })
vim.keymap.set("n", "  ", telescope_builtin("buffers"), { desc = "[ ] Find existing buffers" })
vim.keymap.set("n", "<leader>/", function()
	setup_telescope()
	require("telescope.builtin").current_buffer_fuzzy_find(require("telescope.themes").get_dropdown({
		winblend = 10,
		previewer = false,
	}))
end, { desc = "[/] Fuzzily search in current buffer" })
vim.keymap.set(
	"n",
	"<leader>f/",
	telescope_builtin("live_grep", {
		grep_open_files = true,
		prompt_title = "Live Grep in Open Files",
	}),
	{ desc = "[F]ind [/] in Open Files" }
)
vim.keymap.set("n", "<leader>fn", function()
	setup_telescope()
	require("telescope.builtin").find_files({ cwd = "~/.dotfiles" })
end, { desc = "[F]ind [N]ix files" })

require("blink.cmp").setup({
	keymap = {
		preset = "none",
		["<C-n>"] = { "select_next", "fallback" },
		["<C-p>"] = { "select_prev", "fallback" },
		["<C-b>"] = { "scroll_documentation_up", "fallback" },
		["<C-f>"] = { "scroll_documentation_down", "fallback" },
		["<C-y>"] = { "accept", "fallback" },
		["<C-Space>"] = { "show", "fallback" },
		["<C-l>"] = { "snippet_forward", "fallback" },
		["<C-h>"] = { "snippet_backward", "fallback" },
	},
	completion = {
		documentation = { auto_show = true },
	},
	sources = {
		default = { "lsp", "path", "snippets" },
	},
	signature = { enabled = true },
})

pcall(require("lsp-file-operations").setup)
require("fidget").setup({})

vim.api.nvim_create_autocmd("LspAttach", {
	group = vim.api.nvim_create_augroup("kickstart-lsp-attach", { clear = true }),
	callback = function(event)
		local map = function(keys, func, desc, mode)
			mode = mode or "n"
			vim.keymap.set(mode, keys, func, { buffer = event.buf, desc = "LSP: " .. desc })
		end
		map("gd", function()
			setup_telescope()
			require("telescope.builtin").lsp_definitions()
		end, "[G]oto [D]efinition")
		map("gD", vim.lsp.buf.declaration, "[G]oto [D]eclaration")
		map("gr", function()
			setup_telescope()
			require("telescope.builtin").lsp_references()
		end, "[G]oto [R]eferences")
		map("gI", function()
			setup_telescope()
			require("telescope.builtin").lsp_implementations()
		end, "[G]oto [I]mplementation")
		map("ga", vim.lsp.buf.code_action, "[G]o [a]ction", { "n", "x" })
		map("gn", vim.lsp.buf.rename, "[G]o re[n]ame")
		map("<leader>D", function()
			setup_telescope()
			require("telescope.builtin").lsp_type_definitions()
		end, "Type [D]efinition")
		map("<leader>ds", function()
			setup_telescope()
			require("telescope.builtin").lsp_document_symbols()
		end, "[D]ocument [S]ymbols")
		map("<leader>ws", function()
			setup_telescope()
			require("telescope.builtin").lsp_dynamic_workspace_symbols()
		end, "[W]orkspace [S]ymbols")

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
			and client_supports_method(client, vim.lsp.protocol.Methods.textDocument_documentHighlight, event.buf)
		then
			local highlight_augroup = vim.api.nvim_create_augroup("kickstart-lsp-highlight", { clear = false })
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
		if client and client_supports_method(client, vim.lsp.protocol.Methods.textDocument_inlayHint, event.buf) then
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
	},
})

local capabilities = require("blink.cmp").get_lsp_capabilities()

for _, server in ipairs({
	"bashls",
	"ccls",
	"cssls",
	"eslint",
	"gopls",
	"html",
	"marksman",
	"nil_ls",
	"pyright",
	"ruby_lsp",
	"terraformls",
	"ts_ls",
}) do
	vim.lsp.config(server, { capabilities = capabilities })
	vim.lsp.enable(server)
end

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

require("conform").setup({
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
	formatters = {
		treefmt_dotfiles = {
			command = "nix",
			args = { "fmt", vim.fn.expand("~/.dotfiles"), "--", "$FILENAME" },
			stdin = false,
			condition = function(_, ctx)
				local root = vim.fs.root(ctx.filename, { "flake.nix", ".git" })
				return root == vim.fn.expand("~/.dotfiles")
			end,
		},
	},
	formatters_by_ft = {
		fish = { "treefmt_dotfiles", "fish_indent", stop_after_first = true },
		javascript = { "treefmt_dotfiles", "prettier", stop_after_first = true },
		json = { "treefmt_dotfiles", stop_after_first = true },
		lua = { "treefmt_dotfiles", "stylua", stop_after_first = true },
		nix = { "treefmt_dotfiles" },
		python = { "isort", "black" },
		sh = { "treefmt_dotfiles" },
		yaml = { "treefmt_dotfiles" },
	},
})

vim.keymap.set("", "<leader>a", function()
	require("conform").format({ async = true, lsp_format = "fallback" })
end, { desc = "Format buffer" })

vim.api.nvim_create_autocmd("VimEnter", {
	once = true,
	callback = function()
		require("todo-comments").setup({ signs = false })
	end,
})

local statusline = require("mini.statusline")
statusline.setup({ use_icons = vim.g.have_nerd_font })
statusline.section_location = function()
	return "%2l:%-2v"
end

vim.api.nvim_create_autocmd("FileType", {
	callback = function(args)
		local ft = args.match
		local lang = vim.treesitter.language.get_lang(ft) or ft
		if not vim.treesitter.language.add(lang) then
			return
		end

		vim.treesitter.start(args.buf, lang)
		vim.bo[args.buf].indentexpr = "v:lua.require'nvim-treesitter'.indentexpr()"
		vim.wo.foldmethod = "expr"
		vim.wo.foldexpr = "v:lua.vim.treesitter.foldexpr()"
	end,
})

vim.opt.foldmethod = "expr"
vim.opt.foldexpr = "v:lua.vim.treesitter.foldexpr()"
vim.opt.foldlevel = 99
vim.opt.foldenable = true
