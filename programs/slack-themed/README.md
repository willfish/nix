# Themed Slack (Linux)

Repackages the pinned proprietary Slack binary with a local, CSS-only theme
integration. This is not a source build or an official Slack client fork. No
Slack bundle, native binary, credentials or workspace data is stored here.

`default.nix` extends nixpkgs' Slack package. `patch.cjs` prepends the theme and
keymap loads to the Electron main entry point and fails if that entry point changes.
`theme.cjs` reads `theme-menu/active/hyprland.conf` under `XDG_STATE_HOME`
(default `~/.local/state`) and inserts user-origin CSS into Slack documents.
Colours and font families follow the active desktop theme within two seconds,
including after navigation. Slack's layout, text sizes, images and icon fonts
are not deliberately replaced.

The hook only styles `app:` documents and HTTPS hosts at `slack.com` or its
subdomains. It exposes no renderer API, reads no Slack profile data, performs no
network requests, and does not disable the sandbox, CSP or web security. Missing
theme state leaves the default styling; invalid updates retain the last valid
style. Government Slack domains are not included.

The same allowlist installs a small Vimium-shaped keymap. It does not load
Vimium. `j` `k` `h` `l` scroll the current conversation or thread by 100 pixels,
`d` and `u` scroll half a pane, `gg` goes to the top of that loaded pane and `G`
to the bottom. That is the current view, not the channel's full history. `f`
shows letter hints for visible controls. `gi` focuses the composer for that
pane, not search. `/` opens Slack workspace search. Composer typing, slash
commands, and Ctrl chords such as Ctrl+/ are left alone. Esc leaves the composer
or cancels hints; otherwise Esc still reaches Slack.

These keys follow the loaded scroll container. Slack's message list is remote
and may ignore that scroll, so `j` and `gg` can fail after a Slack update even
though the desktop package has not changed.

## Build and use

Linux desktop work profiles install this package instead of stock Slack:

```sh
direnv exec . nix build .#slack-themed
hmswitch
```

Fully quit Slack, including its tray process, and reopen it after activation.
Subsequent palette changes do not require a restart.

To disable theming or the keymap for a launch, first quit Slack, then run one
of:

```sh
env SLACK_DOTFILES_THEME=0 slack
env SLACK_DOTFILES_KEYS=0 slack
```

The switches are independent. Disabling keys leaves the palette in place.

For a persistent rollback, replace the themed import in
`home/user/packages.nix` with `slack`, run `hmswitch`, and restart Slack.

## Offline check

On a Wayland desktop, run the native-renderer smoke check against the built
package. It uses a disposable copy of Slack's Electron runtime, a synthetic
Slack-shaped page, a separate profile and theme state, and blocked networking.
It does not sign into a workspace or restart an existing Slack process.

```sh
direnv exec . nix shell nixpkgs#asar -c bash programs/slack-themed/smoke.sh \
  "$(readlink -f result)"
```

The check exercises palette/font application, focus, live switching, reload,
invalid-theme recovery, URL boundaries, and the keymap on a native scroller.
It prints the directory containing active-palette and light-mode screenshots.
Inspect those images; remove that temporary directory when finished.

Slack serves the workspace UI remotely. Its CSS classes and tokens can change
independently of the desktop package, so these overrides are best-effort. The
offline check cannot establish complete coverage of the live workspace, huddles,
canvas, accessibility, virtual-list scrolling, or future Slack updates. Review
the actual workspace after upgrades, and disable the hook if it interferes with
normal use.
