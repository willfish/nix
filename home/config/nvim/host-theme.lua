-- Match the host palette; Neovim's terminal background detection selects mode.
local M = {}

function M.setup()
	local path = vim.fn.stdpath("config") .. "/host-palettes.json"
	local palettes = vim.json.decode(table.concat(vim.fn.readfile(path), "\n"))
	local applying = false
	local function apply()
		if applying then
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
	apply()
end

return M
