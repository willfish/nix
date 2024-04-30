return {
	"nvim-telescope/telescope.nvim",
	branch = "0.1.x",
	dependencies = {
		"nvim-lua/plenary.nvim",
		{ "nvim-telescope/telescope-fzf-native.nvim", build = "make" },
    "nvim-telescope/telescope-github.nvim",
    "xiyaowong/telescope-emoji.nvim",
		"nvim-tree/nvim-web-devicons",
    "nvim-telescope/telescope-live-grep-args.nvim",
    "tsakirist/telescope-lazy.nvim",
	},
	config = function()
		local telescope = require("telescope")
		local actions = require("telescope.actions")
		local builtin = require("telescope.builtin")

    telescope.setup {
      defaults = {
        file_ignore_patterns = { "node_modules", "package-lock.json" },
        file_sorter = require("telescope.sorters").get_fzy_sorter,
        prompt_prefix = " > ",
        color_devicons = true,
        file_previewer = require("telescope.previewers").vim_buffer_cat.new,
        grep_previewer = require("telescope.previewers").vim_buffer_vimgrep.new,
        qflist_previewer = require("telescope.previewers").vim_buffer_qflist.new,
        mappings = {
          i = {
            ["<C-x>"] = false,
            ["<C-q>"] = require("telescope.actions").send_to_qflist
          }
        }
      },
      extensions = {
        fzy_native = {
          override_generic_sorter = false,
          override_file_sorter = true
        }
      }
    }

    require("telescope").setup {
      extensions = {
        emoji = {
          action = function(emoji)
            print(emoji.value)
            vim.fn.setreg("+", emoji.value)
            vim.api.nvim_put({ emoji.value }, 'c', false, true)
          end,
        }
      },
    }

    require("telescope").load_extension("gh")
    require("telescope").load_extension("fzy_native")
    require("telescope").load_extension("live_grep_args")
    require("telescope").load_extension("emoji")

    local rg_args =
    "{find_command = {'rg', '--files', '--hidden', '--follow', '--glob', '!.git', '--glob', '!.svn', '--glob', '!.hg', '--glob', '!.bzr', '--glob', '!.tmp', '--glob', '!.DS_Store', '--glob', '!.gitignore', '--glob', '!.gitmodules', '--glob', '!.gitattributes', '--glob', '!.gitkeep', '--glob', '!.gitconfig', '--glob', '!temp_dirs'}}"

    vim.api.nvim_set_keymap(
      "n",
      "<C-f>",
      ":lua require('telescope.builtin').find_files(" .. rg_args .. ")<CR>",
      default_map_opts
    )

    vim.api.nvim_set_keymap(
      "n",
      "<C-g>",
      ":lua require('telescope').extensions.live_grep_args.live_grep_args(" .. rg_args .. ")<CR>",
      default_map_opts
    )

    vim.api.nvim_set_keymap(
      "n",
      "<C-b>",
      ":lua require('telescope.builtin').buffers({sort_lastused = true})<CR>",
      default_map_opts
    )

    vim.api.nvim_set_keymap(
      "n",
      "<C-e>",
      ":lua require('telescope').extensions.emoji.emoji()<CR>",
      default_map_opts
    )

    vim.api.nvim_set_keymap(
      "n",
      "<Leader>fp",
      ":lua require('telescope').extensions.gh.pull_request()<CR>",
      default_map_opts
    )

    vim.api.nvim_set_keymap(
      "n",
      "<Leader>fe",
      ":lua require('telescope').extensions.gh.run()<CR>",
      default_map_opts
    )

    telescope.load_extension("fzf")

    -- set keymaps
    local keymap = vim.keymap -- for conciseness

    keymap.set("n", "<leader>pf", builtin.find_files, {})
    keymap.set("n", "<C-p>", builtin.git_files, {})
    keymap.set("n", "<leader>ps", function()
      builtin.grep_string({ search = vim.fn.input("Grep > ") })
    end)
    keymap.set("n", "<leader>vh", builtin.help_tags, {})
  end,
}
