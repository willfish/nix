# Wallpaper cycling

`wallpaper-cycle` advances the selected theme's packaged backgrounds. The
`hypr-wallpaper-cycle [next|previous]` session wrapper restarts swaybg only after
a successful change. A hand-set wallpaper disables rotation and removes its
previous rotation files and Nix output link.

The helper preserves catalogue ordering, starts after the preferred image when
there is no current selection, and wraps in either direction. It builds the
selected theme lazily through `nix build --out-link`. Background files must
resolve directly inside the theme's canonical `backgrounds` directory.
ImageMagick converts the chosen file to a temporary PNG; a changed package link
or current selection abandons that conversion before publication. The current
filename is saved with mode 0600. Exit codes are 0 for a change, 3 for no change,
1 for an operation failure and 2 for invalid arguments.

## Manual fixtures

No fixture runs through the flake, package build or hooks. From the repository
root, with its direnv environment active:

```sh
meson setup /tmp/wallpaper-cycle-checks programs/wallpaper-cycle \
  -Dfixtures=true --buildtype=debugoptimized
meson compile -C /tmp/wallpaper-cycle-checks
/tmp/wallpaper-cycle-checks/wallpaper-checks
```

For a separate sanitizer build:

```sh
nix develop .#wallpaper-cycle -c bash -c '
  meson setup /tmp/wallpaper-cycle-asan programs/wallpaper-cycle \
    --buildtype=debugoptimized -Dfixtures=true \
    -Db_sanitize=address,undefined -Db_lundef=false
  ninja -C /tmp/wallpaper-cycle-asan
'
/tmp/wallpaper-cycle-asan/wallpaper-checks
```
