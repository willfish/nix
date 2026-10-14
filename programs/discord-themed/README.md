# Themed Discord (Linux)

Repackages nixpkgs' pinned proprietary Discord binary with a CSS-only desktop
palette hook. It is not a Discord source build, protocol replacement, Vencord
installation or Nitro feature unlock. No Discord bundles, account data or
credentials are stored in this repository.

`patch.cjs` adds one module load before the Electron bootstrap and fails the build
if its entry point changes. `theme.cjs` reads the atomically published
`theme-menu/active/hyprland.conf` under `XDG_STATE_HOME` (normally
`~/.local/state`) and replaces user-origin CSS within two seconds. It reapplies
after page reloads and to new app windows. Backgrounds, surfaces, text, accents,
selection and font families follow the selected palette. Muted labels and
button/selection text use contrast-aware colours. Layout, font sizes, images,
role colours and status semantics are not deliberately replaced.

Only HTTPS documents on the exact `discord.com` and legacy `discordapp.com`
origins are styled. Activity frames and external origins are not included. The
hook exposes no renderer API, reads no Discord profile data, makes no network
requests and does not alter preloads, sandbox settings, context isolation or CSP.
Missing state at startup leaves stock styling; missing or invalid updates retain
the last good CSS. The loading splash is unchanged.

## Build and use

```sh
direnv exec . nix build .#discord-themed
hmswitch
```

Fully quit Discord, including its tray process, then reopen it once after
activation. Subsequent palette changes do not need a restart. Select
**User Settings → Appearance → Sync with computer** for Discord's own light/dark
behaviour to follow the desktop. The hook sets CSS colour scheme locally; it does
not rewrite account preferences or enable paid themes.

This normally repackages downloaded binaries rather than compiling Chromium.
Check `nix build --dry-run .#discord-themed` before a costly rebuild. Andromeda can
build the same locked revision when useful; graphical verification and activation
still belong on the host that will run Discord.

## Disable or roll back

After fully quitting Discord, launch without the hook:

```sh
env DISCORD_DOTFILES_THEME=0 discord
```

For persistent rollback, restore `discord` in the Linux desktop package list in
`home/user/packages.nix`, run `hmswitch`, then restart Discord. Account data is
neither migrated nor removed.

## Offline native-renderer check

On a Wayland desktop:

```sh
direnv exec . nix shell nixpkgs#asar -c bash programs/discord-themed/smoke.sh \
  "$(readlink -f result)"
```

The check copies the built runtime, invokes its packaged startup hook, serves
synthetic Discord-shaped documents locally, and blocks other network requests.
Profile, cache and theme state live in a disposable output directory. It checks
palette/fonts, contrast, focus, atomic theme changes, reload, new windows,
invalid-state recovery and origin boundaries. Inspect the printed active and
light screenshots, then remove that directory. Running the same command with
`DISCORD_DOTFILES_THEME=0` checks the launch-time disable switch.

This cannot establish coverage of the actual remotely delivered UI, voice,
screen sharing, accessibility or later Discord releases. Review the real app
with both a light and a dark palette after upgrades, and disable the hook if
styling interferes with normal use. Prefer semantic CSS variables over hashed
class selectors when repairing colour mappings.

## Policy and maintenance risk

[Discord's terms](https://discord.com/terms) restrict software modification and
reverse engineering except with written consent or where applicable law permits
it. A CSS-only hook is still an unofficial modification, not an exemption or a
guarantee against account enforcement. Use the stock client if that risk is
unacceptable. Supported system-mode sync and manual Nitro theme customisation
are alternatives, but do not provide this local palette integration.

Discord delivers the application UI remotely. Its colour tokens can change
independently of the pinned desktop package, so this integration is best-effort.
Keep stock Discord available as a rollback and do not commit extracted binaries
or private screenshots.
