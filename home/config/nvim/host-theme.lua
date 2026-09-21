-- Match the host palette; Neovim's terminal background detection selects mode.
local M = {}

local function palette_path()
	return vim.fn.stdpath("config") .. "/host-palettes.json"
end

local function read_palettes()
	local ok, palettes = pcall(function()
		return vim.json.decode(table.concat(vim.fn.readfile(palette_path()), "\n"))
	end)
	if not ok then
		return nil
	end
	return palettes
end

function M.background()
	local palettes = read_palettes()
	local palette = palettes and palettes[vim.o.background]
	local colour = palette and palette.base00
	if type(colour) ~= "string" then
		return nil
	end
	return colour
end

local function apply_plugin_highlights(palette)
	if type(palette.base0D) == "string" then
		vim.api.nvim_set_hl(0, "NvimTreeFolderArrowClosed", { fg = palette.base0D })
		vim.api.nvim_set_hl(0, "NvimTreeFolderArrowOpen", { fg = palette.base0D })
	end
	if package.loaded["notify"] and type(palette.base00) == "string" then
		require("notify").setup({ background_colour = palette.base00 })
	end
end

function M.setup()
	local path = palette_path()
	local applying = false
	local function apply()
		if applying then
			return
		end
		local palettes = read_palettes()
		if not palettes or not palettes[vim.o.background] then
			return
		end
		local palette = palettes[vim.o.background]
		applying = true
		require("mini.base16").setup({ palette = palette })
		apply_plugin_highlights(palette)
		applying = false
	end
	local group = vim.api.nvim_create_augroup("HostTheme", { clear = true })
	vim.api.nvim_create_autocmd("OptionSet", {
		group = group,
		pattern = "background",
		callback = apply,
	})
	-- Poll the stable path, not the inode replaced by an atomic palette update.
	-- One lightweight stat per second also handles symlinked HM configurations.
	if M.watcher then
		M.watcher:stop()
		M.watcher:close()
	end
	M.watcher = assert(vim.uv.new_fs_poll())
	M.watcher:start(
		path,
		1000,
		vim.schedule_wrap(function(err)
			if not err then
				apply()
			end
		end)
	)
	vim.api.nvim_create_autocmd("VimLeavePre", {
		group = group,
		callback = function()
			if M.watcher then
				M.watcher:stop()
				M.watcher:close()
				M.watcher = nil
			end
		end,
	})
	apply()
end

return M
