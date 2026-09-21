"""Exercise the real Neovim event loop against atomic palette replacements."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NvimPaletteRuntimeTest(unittest.TestCase):
    def test_live_replacement_mode_change_and_watcher_cleanup(self):
        binary = shutil.which("nvim")
        self.assertIsNotNone(
            binary, "Neovim is required for palette runtime checks"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / "config/nvim"
            config.mkdir(parents=True)
            palettes = config / "host-palettes.json"
            palettes.write_text(
                json.dumps(
                    {
                        "dark": {"base00": "#112233", "base0D": "#445566"},
                        "light": {"base00": "#ddeeff", "base0D": "#778899"},
                    }
                )
            )
            script = root / "test.lua"
            script.write_text(f"""
package.preload["mini.base16"] = function()
  return {{ setup = function(options)
    vim.api.nvim_set_hl(0, "Normal", {{ bg = options.palette.base00 }})
  end }}
end
vim.o.background = "dark"
local module = dofile(
  {json.dumps(str(ROOT / "home/config/nvim/host-theme.lua"))}
)
local path = vim.fn.stdpath("config") .. "/host-palettes.json"
local function background()
  return vim.api.nvim_get_hl(0, {{name="Normal"}}).bg
end
local function arrow()
  return vim.api.nvim_get_hl(0, {{name="NvimTreeFolderArrowClosed"}}).fg
end
module.setup()
assert(background() == 0x112233)
assert(arrow() == 0x445566)
vim.wait(200, function() return false end)
for _, colour in ipairs({{0x223344, 0x334455}}) do
  local palette = {{
    dark={{base00=string.format("#%06x", colour), base0D="#445566"}},
    light={{base00="#ddeeff", base0D="#778899"}}
  }}
  vim.fn.writefile({{vim.json.encode(palette)}}, path .. ".new")
  assert(vim.uv.fs_rename(path .. ".new", path))
  assert(vim.wait(3500, function() return background() == colour end),
    "palette did not reload")
end
local arrow_palette = {{
  dark={{base00="#334455", base0D="#123456"}},
  light={{base00="#ddeeff", base0D="#778899"}}
}}
vim.fn.writefile({{vim.json.encode(arrow_palette)}}, path .. ".new")
assert(vim.uv.fs_rename(path .. ".new", path))
assert(vim.wait(3500, function() return arrow() == 0x123456 end),
  "arrow colour did not reload")
vim.o.background = "light"
-- OptionSet is suppressed during startup, so explicitly emit the event.
vim.api.nvim_exec_autocmds("OptionSet", {{pattern="background"}})
assert(background() == 0xddeeff)
assert(arrow() == 0x778899)
local old = module.watcher
module.setup()
assert(old:is_closing(), "setup leaked a watcher")
vim.api.nvim_exec_autocmds("VimLeavePre", {{}})
assert(module.watcher == nil)
vim.cmd("qa!")
""")
            result = subprocess.run(
                [binary, "--headless", "-u", "NONE", "-l", str(script)],
                env={
                    **os.environ,
                    "HOME": directory,
                    "XDG_CONFIG_HOME": str(root / "config"),
                    "NVIM_APPNAME": "nvim",
                },
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertEqual(
                result.returncode, 0, result.stdout + result.stderr
            )


if __name__ == "__main__":
    unittest.main()
