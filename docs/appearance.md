# Host appearance

`home/user/themes/palettes.nix` owns the host palettes. Each has a dark variant
and a warm, lower-brightness light variant. Host identity is independent of mode:
Andromeda uses Rosé Pine, Foundation Tokyo Night, Starfish Solarized, Terminus
Catppuccin, and Relay Gruvbox. Unknown hosts use Andromeda's palette.

## Choosing a mode

Use **COSMIC Settings → Desktop → Appearance** on Linux, or the system appearance
setting on macOS. Dark is the initial default; sunrise/sunset switching is off.
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
- `home/user/themes/render.nix` generates Herdr, Ghostty and Pi colours.
- `home/user/themes/cosmic.py` produces ThemeBuilder inputs. The pinned
  `cosmic-settings appearance import` CLI builds complete themes in an isolated
  Nix sandbox. There is no custom reimplementation of COSMIC's component styling.
- COSMIC owns GTK/Qt exports on graphical Linux. Fixed Stylix GTK, Qt and KDE
  overrides are disabled there to avoid conflicting colour sources.
- Fish and tmux use terminal ANSI roles, with no shell-emitted palette overrides.
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

For a live smoke test, switch dark → light → dark in system settings with Pi
inside Herdr. Check the terminal background, Herdr borders, Pi text and desktop
controls together. Verify remote propagation separately before assuming every
SSH or nested-multiplexer path supports it.
