# Desktop appearance

The theme menu offers all 22 built-in themes from the pinned
[Omarchy source](https://github.com/omacom/omarchy/tree/28ceaae70ebac3a0edcc21f2faa77a90dc6d404c/themes),
using their plain names and native colours. There are no personal palette variants.
Their colour roles are mapped into the existing shared application renderers;
Omarchy's application configurations, scripts and keybindings are not installed.

Host defaults are Rosé Pine on Andromeda, Tokyo Night on Foundation, Osaka Jade
on Starfish, Catppuccin on Terminus and Gruvbox on Relay. Other hosts use Rosé
Pine. A previously saved Solarized selection migrates to Osaka Jade.

## Choosing a theme

On graphical Linux, **Super+Shift+T** opens the Fuzzel theme menu. The current
selection is marked with `*`; Escape leaves it unchanged. **Host default** clears
the local override. Named selections survive Home Manager activation.

Use plain command-line IDs, for example `theme-menu nord`,
`theme-menu catppuccin-latte` or `theme-menu default`.
`theme-menu --reapply` restores the saved selection's generated files.

Each theme owns its native light/dark mode. Catppuccin Latte, Flexoki Light,
Lupine, Rosé Pine and White are light themes; the others are dark. Select another
theme to change mode. There is no synthetic light/dark variant or mode-toggle
row. An incompatible `theme-menu light` or `theme-menu dark` request is rejected.
Use this menu rather than COSMIC Settings to keep mode and palette aligned.

## Wallpapers and applications

In Hyprland, each theme uses its first sorted upstream wallpaper, matching
Omarchy's default selection order. Swaybg displays it on all outputs and follows
the Hyprland session only. Changing the theme replaces the wallpaper too.

`home/config/hyprland/settings.nix` controls the default palette, fonts,
Base16 overrides and wallpaper sizing. For a custom image, set an absolute path
such as `wallpaper.overrides.tokyo-night.dark = "/home/william/Pictures/sky.png";`.
The file must exist when applying the theme. This does not change COSMIC's
wallpaper or install a wallpaper on the greeter.

The shared colours feed Ghostty, Neovim, Herdr and the desktop shell. COSMIC
Greeter reads the selected user's theme and native mode. In Hyprland, the picker
also updates Waybar, Fuzzel, Hyprlock, Mako and GTK appearance. In Brave, select
**Settings → Appearance → Use GTK**. Explicit browser themes, extensions and
website styling can override desktop colours.

Ghostty reloads through its D-Bus action; Neovim instances with the installed
watcher update automatically. Older Neovim instances need restarting once.
The addressed Herdr server reloads; other servers need their own
`herdr server reload-config` with the appropriate socket selected.
Existing Pi sessions need restarting for named palette changes because their
watcher does not watch launcher-loaded files. Explicit Pi theme choices take
precedence. Remote hosts retain their own palettes.

## Ownership

- `home/user/themes/omarchy-source.nix` pins the upstream source and hash.
- `home/user/themes/palettes.nix` projects upstream colour roles into Base16.
- `home/user/themes/omarchy.nix` supplies wallpapers and the upstream licence.
  The licence is installed at `~/.local/share/theme-menu/omarchy-LICENSE`.
- `home/user/appearance.nix` chooses host defaults and wires application config.
- `home/user/themes/runtime.nix` builds the catalogue. Active files and the saved
  selection live under `~/.local/state/theme-menu/`.
- `home/config/appearance/theme_menu.py` publishes selected files and native mode.
  Writes are individually atomic with rollback on failure, not a cross-app
  transaction; applications may update at slightly different times.
- `home/user/themes/cosmic.py` supplies the native COSMIC importer with colour
  inputs. It does not reimplement COSMIC component styling.

Fixed Stylix overrides remain disabled for shared consumers. Custom GTK CSS and
unrelated symlinks are preserved rather than overwritten. Other Stylix targets,
macOS and headless hosts retain their declarative palette rather than following
the graphical Linux menu. See [Hyprland](hyprland.md) for session controls.
