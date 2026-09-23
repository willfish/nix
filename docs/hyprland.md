# Hyprland

Select **Hyprland**, not **Hyprland (uwsm-managed)**, at login. COSMIC remains
available as a fallback. Like the COSMIC setup, editable settings live under
`home/config/` and Home Manager wiring under `home/user/`.

## Configuration

Edit `home/config/hyprland/settings.nix` for keybindings, fonts, colours, gaps,
tiling/floating rules, launcher, rail and session menu settings. Run `hmswitch`
to apply changes. Do not edit the generated `~/.config/hypr/hyprland.conf`.

Your chosen COSMIC shortcuts carry across: Super+WASD focus, Super+Shift+WASD
movement, Super+1–9 workspaces, Super+Shift+1–9 moving windows, Super+Q close,
Super+F fullscreen, Super+G floating, Super or Super+X launcher,
Super+Shift+T appearance and Ctrl+Shift+S screenshots. Voice chords retain their
host capability checks. Super+Shift+B uses Hyprland DPMS rather than COSMIC's
output toggle. Dwindle tiling is not COSMIC's tiler; its behaviour is configured
explicitly.

## Screenshots

Ctrl+Shift+S runs **Grimblast → Satty** directly. Drag to select a region or
click a window, then annotate, crop or redact. Enter saves, copies and closes;
Ctrl+S saves; Escape closes. No unedited screenshot is saved by the capture
pipeline. Cover secrets with an opaque filled rectangle rather than blur.

Images save to `~/Pictures/Screenshots`. The directory and native Satty options
are configured in `settings.nix`. Selection belongs to Hyprland's Grimblast;
editing belongs to Satty. There is no custom screenshot implementation.

This follows Omarchy's approach of delegating capture to an established utility
and keeping it on a convenient shortcut, without adopting its keymap, updater
or shell configuration. Current Omarchy uses Omasnap; this configuration uses
Grimblast and Satty from the pinned Nix packages.

## Appearance and login

Super+Shift+T uses the existing shared palette menu for Ghostty, Neovim and Herdr,
plus Hyprland, Waybar, Fuzzel, Mako and Hyprlock. Use this menu in Hyprland to
refresh desktop consumers, rather than changing appearance in COSMIC Settings.

`appearance.palette` selects the default by host key or palette ID; a saved menu
selection takes precedence. `appearance.mode = null` preserves light/dark mode;
setting `"light"` or `"dark"` reapplies it at activation and session start.
`appearance.paletteOverrides.light` and `.dark` accept Base16 overrides such as
`base00 = "191724";`. Fonts must be installed. Neovim and Herdr inherit their
terminal font. See [shared appearance](appearance.md) for application overrides.

COSMIC Greeter continues reading the selected user's shared theme and mode.
For Brave, select **Settings → Appearance → Use GTK**. Browser extensions,
explicit themes and website styling can override desktop colours.

## First login and recovery

The system and Home Manager configurations must both be activated. After login,
check `hyprctl configerrors`, your shortcuts, screenshot editing, theme changes,
and lock/unlock before relying on idle locking or suspend. Check browser screen
sharing through the portal. These need a real Hyprland session.

If the session is unusable, select COSMIC at the next login or the previous NixOS
generation at boot. Do not restart the display manager inside an active session.
See [host operations](nixos-host-operations.md) for activation and rollback.
