# Themed Slack (Linux)

Repackages the pinned proprietary Slack binary with a local, CSS-only theme
integration. This is not a source build or an official Slack client fork. No
Slack bundle, native binary, credentials or workspace data is stored here.

`default.nix` extends nixpkgs' Slack package. `patch.cjs` prepends one module load
to the Electron main entry point and fails if that entry point changes.
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

## Build and use

Linux desktop work profiles install this package instead of stock Slack:

```sh
direnv exec . nix build .#slack-themed
hmswitch
```

Fully quit Slack, including its tray process, and reopen it after activation.
Subsequent palette changes do not require a restart.

To disable the hook for a launch, first quit Slack, then run:

```sh
env SLACK_DOTFILES_THEME=0 slack
```

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
invalid-theme recovery and URL boundaries. It prints the directory containing
active-palette and light-mode screenshots. Inspect those images; remove that
temporary directory when finished.

Slack serves the workspace UI remotely. Its CSS classes and tokens can change
independently of the desktop package, so these overrides are best-effort. The
offline check cannot establish complete coverage of the live workspace, huddles,
canvas, accessibility or future Slack updates. Review the actual workspace after
upgrades, and disable the hook if styling interferes with normal use.
