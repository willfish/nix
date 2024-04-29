local opts = { noremap = true, silent = true }

vim.keymap.set("n", "<space>e", vim.diagnostic.open_float, opts)
vim.keymap.set("n", "[d", vim.diagnostic.goto_prev, opts)
vim.keymap.set("n", "]d", vim.diagnostic.goto_next, opts)
vim.keymap.set("n", "<space>q", vim.diagnostic.setloclist, opts)

local on_attach = function(client, bufnr)
    if client.name == "tsserver" then
        require('lsp-setup.utils').disable_formatting(client)
    end

    -- Enable completion triggered by <c-x><c-o>
    vim.api.nvim_buf_set_option(bufnr, "omnifunc", "v:lua.vim.lsp.omnifunc")

    -- Mappings.
    local bufopts = { noremap = true, silent = true, buffer = bufnr }

    vim.keymap.set("n", "gD", vim.lsp.buf.declaration, bufopts)
    vim.keymap.set("n", "gd", vim.lsp.buf.definition, bufopts)
    vim.keymap.set("n", "gi", vim.lsp.buf.implementation, bufopts)
    vim.keymap.set("n", "K", vim.lsp.buf.hover, bufopts)
    vim.keymap.set("n", "<C-k>", vim.lsp.buf.signature_help, bufopts)
    vim.keymap.set("n", "<space>wa", vim.lsp.buf.add_workspace_folder, bufopts)
    vim.keymap.set("n", "<space>wr", vim.lsp.buf.remove_workspace_folder, bufopts)
    vim.keymap.set(
        "n",
        "<space>wl",
        function()
            print(vim.inspect(vim.lsp.buf.list_workspace_folders()))
        end,
        bufopts
    )
    vim.keymap.set("n", "<space>D", vim.lsp.buf.type_definition, bufopts)
    vim.keymap.set("n", "gn", vim.lsp.buf.rename, bufopts)
    vim.keymap.set("n", "<leader>ga", vim.lsp.buf.code_action, bufopts)
    vim.keymap.set("v", "<leader>ga", vim.lsp.buf.code_action, bufopts)
    vim.keymap.set("n", "gr", vim.lsp.buf.references, bufopts)
    vim.keymap.set(
        "n",
        "<leader>a",
        function()
            vim.lsp.buf.format { async = true }
        end,
        bufopts
    )
end

require("nvim-lsp-installer").setup {}

require('neodev').setup({
    library = {
        enabled = true,
        runtime = true,
        types = true,
        plugins = true,
    },
    setup_jsonls = true,
    lspconfig = true,
    pathStrict = true,
})

require("lsp-setup").setup(
    {
        on_attach = on_attach,
        capabilities = vim.lsp.protocol.make_client_capabilities(),
        servers = {
            ["bashls"] = {},
            ["rust_analyzer@nightly"] = {
                settings = {
                    ["rust-analyzer"] = {
                        imports = {
                            granularity = {
                                group = "module"
                            },
                            prefix = "self"
                        },
                        cargo = {
                            buildScripts = {
                                enable = true
                            }
                        },
                        procMacro = {
                            enable = true
                        }
                    }
                }
            },
            ["pyright"] = {},
            ["gopls"] = {},
            ["solargraph"] = {},
            ["html"] = {},
            ["terraformls"] = {},
            -- ["jsonls"] = {
            --     settings = {
            --         json = {
            --             schemas = require("schemastore").json.schemas(),
            --             validate = { enable = true }
            --         }
            --     }
            -- },
            -- ["yamlls"] = {
            --     settings = {
            --         yaml = {
            --             format = {
            --                 enable = true
            --             },
            --             schemaStore = {
            --                 url = "https://www.schemastore.org/api/json/catalog.json",
            --                 enable = true
            --             }
            --         }
            --     }
            -- },
            ["lua_ls"] = {
                Lua = {
                    runtime = {
                        version = "LuaJIT",
                        path = vim.split(package.path, ";"),
                    },
                    diagnostics = {
                        globals = {
                            "vim",
                            "nnoremap",
                            "vnoremap",
                            "inoremap",
                            "tnoremap",
                            "use",
                        },
                    },
                    workspace = {
                        checkThirdParty = false,
                        library = {
                            [vim.fn.expand("$VIMRUNTIME/lua")] = true,
                            [vim.fn.expand("$VIMRUNTIME/lua/vim/lsp")] = true,
                        },
                    },
                }
            },
            ["tsserver"] = {
                settings = {
                    ["typescript"] = {
                        format = {
                            enable = false
                        },
                        diagnostics = {
                            ignoredCodes = { 6133, 6138, 6139, 6140 }
                        }
                    }
                }
            },
            ["cssls"] = {},
            ["vimls"] = {},
            ["dockerls"] = {},
            ["sqlls"] = {
                settings = {
                    sql = {
                        format = {
                            enable = true
                        }
                    }
                }
            },
            ["clangd"] = {},
            ["awk_ls"] = {},
            ["marksman"] = {},
            ["nil"] = {},
        }
    }
)
