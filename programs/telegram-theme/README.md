# Telegram desktop theme

`home/user/themes/telegram.nix` maps each desktop palette to Telegram's native
colour roles. This derivation adds a matching solid-colour chat wallpaper and
writes a reproducible `.tdesktop-theme` ZIP with fixed timestamps and stripped
image metadata.

The theme catalogue publishes the selected archive atomically at
`theme-menu/active/telegram.tdesktop-theme` under `XDG_STATE_HOME`
(default `~/.local/state`). The Linux Home Manager desktop profile installs
`programs/telegram-themed`, which loads and follows that path automatically.
There is no import step, local-automation permission or profile-file editing.

The palette changes colours and the theme's default chat wallpaper, not fonts,
layout, media or per-chat custom backgrounds. See
[the client integration](../telegram-themed/README.md) for disabling it.

## Check

Build the affected Home Manager configuration and verify the published ZIP has
`colors.tdesktop-theme` and `background.png`. Check generated role names against
the pinned Telegram source's `Telegram/lib_ui/ui/colors.palette`, and verify the
wallpaper pixel matches the palette's background.

In the patched app, change the desktop palette and verify Telegram updates
without a restart. Exercise light and dark modes, including Telegram's automatic
night-mode handling, then restore the original desktop selection.
