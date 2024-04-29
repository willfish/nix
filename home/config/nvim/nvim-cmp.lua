local cmp = require("cmp")

cmp.setup(
    {
        snippet = {
            expand = function(args)
                require("luasnip").lsp_expand(args.body) -- For `luasnip` users.
            end
        },
        window = {},
        formatting = {
            fields = { "menu", "abbr", "kind" },
            format = function(entry, item)
                local menu_icon = {
                    nvim_lsp = "λ",
                    luasnip = "⋗",
                    buffer = "Ω",
                    path = "🖫"
                }

                item.menu = menu_icon[entry.source.name]
                return item
            end
        },
        mapping = cmp.mapping.preset.insert(
            {
                ["<C-l>"] = cmp.mapping.confirm({ select = true })
            }
        ),
        sources = cmp.config.sources(
            {
                { name = "nvim_lsp" },
                { name = "luasnip" },
                { name = "path" }
            },
            {
                { name = "buffer" }
            }
        )
    }
)

require("luasnip.loaders.from_snipmate").lazy_load()

-- Set configuration for specific filetype.
cmp.setup.filetype(
    "gitcommit",
    {
        sources = cmp.config.sources(
            {
                { name = "cmp_git" }
            },
            {
                { name = "buffer" }
            }
        )
    }
)

-- Use cmdline & path source for ':' (if you enabled `native_menu`, this won't work anymore).
cmp.setup.cmdline(
    ":",
    {
        mapping = cmp.mapping.preset.cmdline(),
        sources = cmp.config.sources(
            {
                { name = "path" }
            },
            {
                { name = "cmdline" }
            }
        )
    }
)
