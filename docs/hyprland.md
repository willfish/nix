# Hyprland

Hyprland is the only supported graphical session. Editable settings live under
`home/config/` and Home Manager wiring under `home/user/`.

SDDM uses Omarchy's QML login theme without reading your home directory. The
precreated file `/var/lib/desktop-theme/william` is mode 0644 and owned by
william. The directory is root-owned mode 0755, so writers must open that file
in place. It holds only a theme ID, validated against the greeter's immutable
theme catalogue. A saved theme-menu selection overrides the host
default in `home/user/themes/host-defaults.nix`. A system service refreshes the
immutable theme link when that selection changes, without restarting SDDM.
Plymouth's matching boot artwork uses `appearance.bootPalette` in the central
settings file, or the host default when null. It requires a system rebuild.
Stage the system generation for the next boot; do not switch it into the running
session. The first reboot activates SDDM and Plymouth. Password login remains
required. The upstream layout offers no user or session chooser; it logs William
into Hyprland. Use a TTY or the previous boot generation for recovery.

## Configuration

Edit `home/config/hyprland/settings.nix` for keybindings, fonts, colours, gaps,
tiling/floating rules, launcher, rail and session menu settings. Run `hmswitch`
to apply changes. Do not edit the generated `~/.config/hypr/hyprland.conf`.

Super+WASD focuses, Super+Shift+WASD moves windows, Super+1–9 switches
workspaces and Super+Shift+1–9 moves windows between them. Super+Q closes,
Super+F toggles fullscreen, Super+G toggles floating, and Super or Super+X
opens the launcher. Super+Shift+T opens appearance and Ctrl+Shift+S takes a
screenshot. Voice chords retain their host capability checks. Super+Shift+B
uses Hyprland DPMS to power the display. Dwindle tiling behaviour is configured
explicitly in `settings.nix`.

## Desktop controls

Waybar stays the only bar. Its audio, Bluetooth and network buttons open the
actual pinned Omarchy Quickshell panels, hosted beside the rail rather than in
Omarchy's full shell. Configure rail modules, commands and native widget overrides
under `bar` in `settings.nix`. Scroll audio or brightness to adjust them;
right-click audio to mute. Right-click playback to open CLIamp.

Bluetooth pairing opens Blueman for confirmation and PIN entry. Advanced network
settings, including DNS and enterprise Wi-Fi profiles, open NetworkManager's
editor rather than running Omarchy's system-wide configuration scripts. QR
sharing, speed tests and the optional media/OSD service are not hosted.

The defaults use Nautilus for folders, Evince for PDFs, Neovim in Ghostty for text,
and mpv with MPRIS for audio/video.

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
plus Hyprland, Waybar, the control panels, Fuzzel, Mako and Hyprlock. Use this menu in Hyprland to
refresh those desktop consumers.

The menu contains all built-in Omarchy themes without a name prefix or personal
variants. Each selection applies its native mode and matching wallpaper.
`appearance.palette` selects the default by theme ID, such as `"nord"`; a saved
menu selection takes precedence. `appearance.paletteOverrides.light` and `.dark`
accept Base16 overrides such as `base00 = "191724";`. `wallpaper.mode` controls
image sizing and `wallpaper.overrides` accepts per-theme, per-mode image paths.
Fonts must be installed. Neovim and Herdr inherit their terminal font. See
[shared appearance](appearance.md) for sources and application overrides.

Default Fuzzel and the voice menu both include
`~/.local/state/theme-menu/active/fuzzel.ini`. Voice menu width and lines come
from `menus.voice` in `settings.nix`. `bar.traySpacing` controls the gap between
tray icons independently of the other bar widgets.

For Brave, select **Settings → Appearance → Use GTK**. Browser extensions,
explicit themes and website styling can override desktop colours.

## First login and recovery

Home Manager can be activated for the user session. Stage the system generation
and reboot once before expecting the new greeter. After that login, check
`hyprctl configerrors`, your shortcuts, screenshot editing, theme changes, and
lock/unlock before relying on idle locking or suspend. Check browser screen
sharing through the portal. These need a real Hyprland session.

If the new generation is unusable, select the previous NixOS generation from
the systemd-boot menu. Login has no alternate desktop. Do not restart the
display manager inside an active session. See
[host operations](nixos-host-operations.md) for activation and rollback.
