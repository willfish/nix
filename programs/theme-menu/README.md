# Theme menu

The C controller preserves `theme-menu --catalogue FILE --state DIR
[--reapply] [--no-reload] [palette]`. The graphical Linux wrapper supplies the
catalogue, state and desktop publishing environment. Omit the palette for Fuzzel;
Escape leaves it unchanged. `default` removes the saved override; `--reapply`
restores it. Persisted Solarized selections migrate to Osaka Jade.

Bundles are read before publication. Ordered writes snapshot all destinations,
replace changed files and symlinks atomically, and roll back on failure before
committing selection. Unchanged regular files retain their inode and permissions.
This is not a cross-application transaction. Native mode belongs to the palette;
other palettes retain `state/mode`. Explicit light/dark actions update projected
session assets without overwriting application overrides or palette selection.
An advisory lock covers the popup, state writes and desktop publication.

`THEME_MENU_PUBLISH=1` enables lazy wallpaper builds, rotation-root management,
greeter updates and Hyprland publication. `THEME_WALLPAPER_FLAKE` selects the
wallpaper flake. Greeter updates never create files or follow symlinks; only an
existing regular file receives a catalogue-listed ASCII ID, including newline,
of at most 64 bytes. GTK CSS symlinks and custom styles remain untouched. Waybar
reloads use the current user's unit, not a process scan. btop reloads use SIGUSR2;
Ghostty uses its public D-Bus action and is not started. Herdr reloads its
addressed server. Desktop command failures produce the original warning/fallback
behavior and deadlines.

For disposable, nonpublishing bundle checks, unset `THEME_MENU_PUBLISH` and use
`--no-reload`. That flag alone does not disable wallpaper reads/builds when the
publishing environment is already enabled.

## Manual fixtures

No automatic test registration, package test phase or flake check is installed.
Only the noninstalled fixture allows temporary greeter/proc paths and atomic-write
failure injection.

```sh
direnv exec . nix develop .#theme-menu -c \
  meson setup /tmp/theme-menu-build programs/theme-menu -Dfixtures=true
direnv exec . nix develop .#theme-menu -c ninja -C /tmp/theme-menu-build
THEME_MENU_BIN=/tmp/theme-menu-build/theme-menu \
THEME_MENU_FIXTURE=/tmp/theme-menu-build/theme-menu-fixture \
  /tmp/theme-menu-build/theme-menu-check
```

Set `THEME_MENU_CATALOGUE` to a built host catalogue and `THEME_MENU_FUZZEL` to the
pinned Fuzzel executable to check every generated bundle and both picker modes.
Optional `THEME_MENU_LEGACY` and `THEME_MENU_PYTHON` enable disposable legacy CLI
comparison, not an installed Python dependency. For ASan/UBSan, use a fresh Meson
build with `-Db_sanitize=address,undefined -Db_lundef=false` and the same driver.
