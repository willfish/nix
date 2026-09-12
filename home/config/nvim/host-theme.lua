-- Match the host palette; Neovim's terminal background detection selects mode.
local M = {}

function M.setup()
	local path = vim.fn.stdpath("config") .. "/host-palettes.json"
	local applying = false
	local function apply()
		if applying then
			return
		end
		local ok, palettes = pcall(function()
			return vim.json.decode(table.concat(vim.fn.readfile(path), "\n"))
		end)
		if not ok or not palettes[vim.o.background] then
			return
		end
		applying = true
		require("mini.base16").setup({ palette = palettes[vim.o.background] })
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
