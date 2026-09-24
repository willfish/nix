# Desktop appearance

The theme menu discovers built-in themes from the `omarchy` flake input and
community themes from data-only inputs named `omarchy-theme-*`. Each theme is
packaged as a Nix derivation with its native colours and supplied artwork.
Colour roles feed the shared application renderers; upstream installation
scripts, application configurations and keybindings are not executed or installed.

Host defaults live in `home/user/themes/host-defaults.nix`: Rosé Pine on
Andromeda, Tokyo Night on Foundation, Osaka Jade on Starfish, Catppuccin on
Terminus and Gruvbox on Relay. Other hosts use Rosé Pine. A saved theme-menu
selection overrides that default. A previously saved Solarized selection
migrates to Osaka Jade.

## Choosing a theme

On graphical Linux, **Super+Shift+T** opens the Fuzzel theme menu. The current
selection is marked with `*`; Escape leaves it unchanged. **Host default** clears
the local override. Named selections survive Home Manager activation.

Use plain command-line IDs, for example `theme-menu nord`,
`theme-menu catppuccin-latte` or `theme-menu default`.
`theme-menu --reapply` restores the saved selection's generated files.

Each theme owns its native light/dark mode. Catppuccin Latte, Flexoki Light,
Lupine, Rosé Pine and White are light built-in themes. Community themes also
include light palettes. Select another theme to change mode. There is no
synthetic light/dark variant or mode-toggle row. An incompatible `theme-menu light` or `theme-menu dark` request is rejected.
Use this menu to keep mode and palette aligned.

## Adding and updating themes

The catalogue includes 145 publicly available repositories from
[Omarchy's extra themes page](https://omarchy.org/themes/), alongside the bundled
themes. Gruvu's listed repository returns 404 and is recorded in
`home/user/themes/community-unavailable.json` rather than breaking every build.
Picker labels are capitalised, preserving names such as NES and Tokyo Night OLED.

Declare additional repositories outside the generated community block in
`flake.nix`, using a slug as the input suffix:

```nix
omarchy-theme-sakura = {
  url = "git+https://github.com/bjarneo/omarchy-sakura-theme?shallow=1";
  flake = false;
};
```

The importer discovers it as `community-sakura`; no palette table is needed.
Shallow Git inputs avoid GitHub's anonymous API rate limit when resolving the
whole catalogue. Every input has `flake = false`: repository code is never
imported as Nix.

The importer reads semantic or legacy ANSI `colors.toml`, falling back to colour
data in `alacritty.toml`. It follows Omarchy's canonical-name, short-name and ANSI
fallbacks. Mode comes from `mode`, then `theme_type`, then a `light.mode` marker
or the background brightness. RGB colours are validated. Alacritty files are
parsed as data, not installed; shell commands, terminal settings and theme
installers are never executed.

```sh
direnv exec . nix flake update omarchy omarchy-theme-sakura
direnv exec . nix build .#theme-community-sakura
hmswitch
theme-menu community-sakura
```

A full `nix flake update` updates all declared theme inputs too. New upstream
built-in directories are discovered automatically; new community repositories
must first be declared as inputs. There is no live website crawl during a build.
All revisions and content hashes live in `flake.lock`.

To refresh the official website list, choose a reviewed `omacom/omarchy-site`
commit and download its HTML as data:

```sh
site_rev=71cd3ed9e83511875ab9472127616989deb68bf5
url="https://raw.githubusercontent.com/omacom/omarchy-site/$site_rev/themes/index.html"
page=$(nix store prefetch-file --json "$url" | jq -r .storePath)
python3 scripts/import-omarchy-community.py "$page" "$url"
direnv exec . nix flake lock
```

The helper refreshes only the marked input block and `community.json`, which
records website names, repository links and the source revision. Review the
diff and the unavailable list, then build and activate Home Manager. Remove an
unavailable entry before regenerating if its repository becomes public again.
The helper does not fetch theme contents or run their scripts.

Theme packages retain supplied licences and all supported background images.
The desktop uses the first sorted image, or a solid native background if none
exists. Missing login artwork falls back to Omarchy's logo recoloured with the
palette's foreground. Adding themes to the desktop needs only Home Manager
activation. The system greeter's allowlist and boot artwork still require a
system rebuild; desktop activation does not restart the display manager.

## Wallpapers and applications

In Hyprland, each theme uses its first sorted upstream wallpaper, matching
Omarchy's default selection order. Swaybg displays it on all outputs and follows
the Hyprland session only. Changing the theme replaces the wallpaper too.

`home/config/hyprland/settings.nix` controls the default palette, fonts,
Base16 overrides and wallpaper sizing. For a custom image, set an absolute path
such as `wallpaper.overrides.tokyo-night.dark = "/home/william/Pictures/sky.png";`.
The file must exist when applying the theme. It is a Hyprland wallpaper, not a
greeter wallpaper.

The shared colours feed Ghostty, Neovim, Herdr, btop and the desktop shell.
Herdr only accepts its built-in theme names. A palette it does not know keeps
those colours as overrides on Catppuccin, or Catppuccin Latte when the palette
is light. The menu and wallpaper still use the palette's own name. In
Hyprland, the picker also updates Waybar, Fuzzel, Hyprlock, Mako and GTK
appearance. Default Fuzzel and the voice menu include
`~/.local/state/theme-menu/active/fuzzel.ini`. Voice menu width and lines come
from `menus.voice` in `home/config/hyprland/settings.nix`.

SDDM uses Omarchy's QML login layout and theme-specific unlock artwork.
It does not read your home directory. The precreated file
`/var/lib/desktop-theme/william` holds only a selected theme ID. A system
service validates it against immutable assets, falls back to the host default
if invalid, and updates the login theme link without restarting your session.
The next greeter uses the updated assets once that service has finished.

Plymouth uses the matching Omarchy boot and disk-unlock artwork. Its palette
comes from `appearance.bootPalette` in `home/config/hyprland/settings.nix`;
`null` uses the host default. Boot artwork is embedded in the initrd, so changing
it requires a system rebuild and reboot. The desktop menu cannot change the
boot image. Neither theme enables automatic login or changes disk encryption.

In Brave, select **Settings → Appearance → Use GTK**. Explicit browser themes,
extensions and website styling can override desktop colours.

btop reads `~/.config/btop/themes/host.theme`. On graphical Linux that file
follows the selected theme, and a running btop reloads with it. Themes that
ship their own `btop.theme` keep that file, so palette overrides do not retint
them. Other themes are rendered from the same colour roles as the rest of the
desktop. The managed btop config does not save changes on exit, so a btop
settings screen cannot replace the selected theme.

Ghostty reloads through its D-Bus action; Neovim instances with the installed
watcher update automatically. Older Neovim instances need restarting once.
The addressed Herdr server reloads; other servers need their own
`herdr server reload-config` with the appropriate socket selected.
Existing Pi sessions need restarting for named palette changes because their
watcher does not watch launcher-loaded files. Explicit Pi theme choices take
precedence. Remote hosts retain their own palettes.

## Ownership

- `flake.nix` declares sources; `flake.lock` pins them.
  `home/user/themes/inputs.nix` resolves those same locked sources for pure
  imports outside the flake module graph.
- `home/user/themes/palettes.nix` discovers the catalogue; `normalize-colours.nix`
  resolves current and legacy palette data, and `import-theme.nix` validates and
  maps native colour roles into Base16 without evaluating repo code.
- `home/user/themes/mk-theme.nix` packages supported assets as derivations.
- `home/user/themes/omarchy.nix` exposes packages, wallpapers and the upstream licence.
  The licence is installed at `~/.local/share/theme-menu/omarchy-LICENSE`.
- `home/user/themes/host-defaults.nix` defines the host default theme IDs.
- `home/user/appearance.nix` wires application config from that default unless a
  saved selection overrides it.
- `home/user/themes/runtime.nix` builds the catalogue. Active files and the saved
  selection live under `~/.local/state/theme-menu/`.
- `home/config/appearance/theme_menu.py` publishes selected files and writes
  native mode to `~/.local/state/theme-menu/mode`, the only light/dark
  authority. Writes are individually atomic with rollback on failure, not a
  cross-app transaction; applications may update at slightly different times.
  When desktop publishing is enabled and `/var/lib/desktop-theme/william`
  already exists, the menu overwrites that regular file in place with the
  selected theme ID. A missing file is ignored. A symlink or non-regular path
  is left unchanged. GTK CSS symlinks and their targets are preserved.

Fixed Stylix overrides remain disabled for shared consumers. Custom GTK CSS and
symlinks are preserved rather than overwritten. Other Stylix targets,
macOS and headless hosts retain their declarative palette rather than following
the graphical Linux menu. See [Hyprland](hyprland.md) for session controls.
