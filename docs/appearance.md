# Host appearance

`home/user/themes/palettes.nix` owns the host palettes. Each has a dark variant
and a warm, lower-brightness light variant. Host identity is independent of mode:
Andromeda uses Rosé Pine, Foundation Tokyo Night, Starfish Solarized, Terminus
Catppuccin, and Relay Gruvbox. Unknown hosts use Andromeda's palette.

## Choosing a palette

On graphical Linux, **Super+Shift+T** opens `theme-menu`, a Fuzzel popup like the
voice menu. Choose Rosé Pine, Tokyo Night, Solarized, Catppuccin or Gruvbox.
The current selection is marked with `*`; Escape leaves it unchanged.

**Host default** is the initial selection and follows the host mapping above.
Named selections are local overrides, preserved across Home Manager switches.
Selecting Host default clears the override. Neither selection changes light/dark
mode or the host's declarative palette.

The command also accepts `default`, `rose-pine`, `tokyo-night`, `solarized`,
`catppuccin` or `gruvbox`, for example `theme-menu rose-pine`.
`theme-menu --reapply` restores generated files for the saved selection.

The picker updates COSMIC's desktop and GTK/Qt exports, reloads Ghostty through
its Linux D-Bus action and reloads the addressed Herdr server. Neovim instances
started after installing this configuration detect changes within about a second.
Restart older Neovim instances once to install the watcher. Existing Pi sessions
need restarting for named palette changes: its upstream watcher does not watch
these launcher-loaded files. Light/dark changes still propagate without restart.

If a reload fails, the notification gives the manual fallback. Additional Herdr
servers need their own `herdr server reload-config` with the relevant socket
selected. A remote host keeps its own palette; this popup does not change remote
configuration. macOS and headless hosts retain their declarative host palettes.

## Choosing a mode

In **Super+Shift+T**, the first action switches to light or dark mode, whichever
is not currently active. It leaves your selected palette unchanged. You can also
run `theme-menu light` or `theme-menu dark`, or use
**COSMIC Settings → Desktop → Appearance**. These controls share the same setting.
On macOS, use the system appearance setting.
Dark is the initial default; sunrise/sunset switching is off.
Home Manager preserves the selected COSMIC mode rather than pinning it to dark.

Ghostty follows system appearance. Herdr follows the terminal and passes its
appearance reports to panes. Pi's launchers select a native `host-light/host-dark`
pair without overwriting saved Pi settings. Explicit `--use-theme` arguments take
precedence; selecting a fixed theme in Pi affects that session. Restart with the
normal launcher to return to system following.

After the first Home Manager switch, open a fresh Ghostty window and restart Pi
sessions to pick up the new launcher. Existing Fish shells may have already set
terminal colour overrides; a fresh terminal avoids carrying those forward.
Subsequent mode changes do not require restarting Pi.

Remote Herdr/Pi keep the remote host's palette. The attached terminal supplies the
mode, not a remote desktop setting. This depends on the terminal chain forwarding
appearance reports; older multiplexers may require a restart or an explicit Pi
`--use-theme host-light` / `--use-theme host-dark` override.

## Ownership

- `home/user/appearance.nix` wires the palette into Home Manager.
- `home/user/themes/runtime.nix` builds the graphical Linux palette catalogue.
  The picker keeps its selection and active application files under
  `~/.local/state/theme-menu/`. Home Manager links application configuration to
  those stable writable files and reapplies the selected bundle after activation.
- `home/config/appearance/theme_menu.py` owns runtime selection and COSMIC theme
  files. It leaves COSMIC mode and unrelated desktop settings untouched. Palette
  writes are individually atomic, with rollback on write failure; there is no
  cross-application transaction, so live updates may briefly arrive separately.
- `home/user/themes/render.nix` generates Herdr, Ghostty and Pi colours.
- `home/user/themes/cosmic.py` produces ThemeBuilder inputs. The pinned
  `cosmic-settings appearance import` CLI builds complete themes in an isolated
  Nix sandbox. There is no custom reimplementation of COSMIC's component styling.
- COSMIC owns GTK/Qt exports on graphical Linux. Fixed Stylix GTK, Qt and KDE
  overrides are disabled there to avoid conflicting colour sources.
- Fish uses terminal ANSI roles, with no shell-emitted palette overrides.
- Git delta uses terminal ANSI green and red for hunks and does not pin a delta theme or light/dark mode. Detection follows the terminal.
- Neovim uses the same palettes, following its detected `background` option.
  `:set background=light` or `:set background=dark` is an explicit fallback when a
  terminal does not update background detection.
- Remaining Stylix targets receive the host's dark palette as a fallback. This
  is not a universal live-switch mechanism for arbitrary third-party apps.

Layout stays deliberately restrained: opaque terminal backgrounds, small window
margins, and a one-column Pi editor inset unless already customised. Thinking,
tool visibility, keyboard shortcuts and conversation layout are unchanged.

## Checks

Build and activate using `docs/nixos-host-operations.md`. Run the complete offline
behavioral gate against the built generation; it includes palette contrast,
host ownership, mode migration, and Pi PTY light/dark switching at narrow/wide
sizes. `nix flake check` covers formatting and module checks.

For a live smoke test, select each named palette and return to Host default.
Check that Escape cancels, the current selection is marked and a Home Manager
switch preserves a named override. Switch dark → light → dark in system settings
with Pi inside Herdr. Check the terminal background, Herdr borders, Pi text and desktop
controls together. Verify remote propagation separately before assuming every
SSH or nested-multiplexer path supports it.
