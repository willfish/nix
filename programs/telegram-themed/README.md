# Telegram with declarative desktop theming

This package builds the pinned open-source Telegram Desktop with a small patch
to its existing native theme loader. The Linux desktop profile installs it in
place of the stock package. No extra service or local-automation access is used.

At startup and every two seconds, it checks the theme archive published by
`theme-menu` under `${XDG_STATE_HOME:-$HOME/.local/state}/theme-menu/active/`.
It applies changed content through Telegram's own theme API. Telegram's native
file watcher also reacts immediately to updates. The timer handles startup,
late creation of the file, atomic replacement and switching day/night slots.

Missing or invalid themes leave the last valid appearance intact. The theme
editor and unconfirmed theme previews suspend the managed override. Closing the
editor or keeping/reverting the preview resumes desktop theming. A manually
selected theme is otherwise replaced by the desktop-managed theme.

Build with `nix build .#telegram-themed`, activate with `hmswitch`, then fully
quit and reopen Telegram once. Later desktop-palette changes need no restart.
The managed colours and default chat wallpaper do not alter messaging,
authentication, fonts, layout, media or per-chat custom backgrounds.

To opt out for a launch, fully quit Telegram and run:

```sh
env TELEGRAM_DOTFILES_THEME=0 Telegram
```

Then choose a different theme in Telegram if desired. For a persistent rollback,
restore `telegram-desktop` in `home/user/packages.nix`, run `hmswitch`, and restart
Telegram. The last selected theme remains in Telegram's normal settings.

Rebase the patch when upgrading Telegram and verify startup plus live dark/light
updates in the native application. The package builds upstream source, rather
than copying or editing an installed binary. Initial builds and Telegram upgrades
are slower because the patched client cannot use the stock binary cache.
