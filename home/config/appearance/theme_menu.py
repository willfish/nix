"""Apply immutable palette bundles to writable, watched application files."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


COSMIC_NAMES = ("Light", "Dark", "Light.Builder", "Dark.Builder")
GTK_CSS = ("gtk-3.0/gtk.css", "gtk-4.0/gtk.css")
# User unit owned by the Hyprland module. systemctl --user stays in this UID.
WAYBAR_UNIT = "waybar.service"


def hyprland_session():
    desktop = os.environ.get("XDG_CURRENT_DESKTOP", "").lower()
    parts = {
        part.strip()
        for part in desktop.replace(":", ";").split(";")
        if part.strip()
    }
    return "hyprland" in parts or bool(
        os.environ.get("HYPRLAND_INSTANCE_SIGNATURE")
    )


def theme_variables(text):
    values = {}
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("$theme_") or "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        values[key[1:].strip()] = value.strip()
    return values


def atomic_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


class Themes:
    def __init__(self, catalogue, state, config):
        self.catalogue, self.state, self.config = (
            catalogue,
            Path(state),
            Path(config),
        )

    def selection(self):
        try:
            selected = (self.state / "selection").read_text().strip()
        except FileNotFoundError:
            return "default"
        self.resolve(selected)
        return selected

    def resolve(self, selected):
        key = self.catalogue["default"] if selected == "default" else selected
        if key not in self.catalogue["palettes"]:
            raise ValueError(
                f"Unknown palette: {selected}. Use theme-menu default to reset."
            )
        return self.catalogue["palettes"][key]

    def apply(self, selected):
        palette = self.resolve(selected)
        # Read the whole bundle before touching active files. Never modify mode.
        writes = {
            self.state / "active" / name: Path(source).read_bytes()
            for name, source in palette["files"].items()
        }
        cosmic = Path(palette["cosmic"]) / "cosmic"
        for variant in COSMIC_NAMES:
            variant_dir = cosmic / f"com.system76.CosmicTheme.{variant}"
            versions = sorted(
                path
                for path in variant_dir.iterdir()
                if path.is_dir() and path.name.startswith("v")
            )
            copied = False
            for source_dir in versions:
                sources = [
                    path for path in source_dir.iterdir() if path.is_file()
                ]
                if not sources:
                    continue
                copied = True
                relative = (
                    Path("cosmic")
                    / f"com.system76.CosmicTheme.{variant}"
                    / source_dir.name
                )
                for source in sources:
                    writes[self.config / relative / source.name] = (
                        source.read_bytes()
                    )
            if not copied:
                raise ValueError(f"Incomplete COSMIC palette: {variant}")
        writes.update(self.projected_writes(self.current_mode(), palette))
        selection_path = self.state / "selection"
        previous_selection = (
            selection_path.read_bytes() if selection_path.exists() else None
        )
        previous = {
            path: path.read_bytes() if path.exists() else None
            for path in writes
        }
        changed = []
        try:
            for path, content in writes.items():
                if previous[path] != content or path.is_symlink():
                    atomic_write(path, content)
                    changed.append(path)
            if selected == "default":
                selection_path.unlink(missing_ok=True)
            else:
                atomic_write(selection_path, (selected + "\n").encode())
        except OSError:
            for path in reversed(changed):
                if previous[path] is None:
                    path.unlink(missing_ok=True)
                else:
                    atomic_write(path, previous[path])
            if previous_selection is None:
                selection_path.unlink(missing_ok=True)
            else:
                atomic_write(selection_path, previous_selection)
            raise

    @property
    def mode_file(self):
        return self.config / "cosmic/com.system76.CosmicTheme.Mode/v1/is_dark"

    def current_mode(self):
        # COSMIC ThemeMode remains authoritative when present, including for
        # the greeter. Hyprland without that file uses theme-menu persistence.
        if self.mode_file.exists():
            return (
                "light"
                if self.mode_file.read_text().strip() == "false"
                else "dark"
            )
        try:
            mode = (self.state / "mode").read_text().strip()
        except FileNotFoundError:
            return "dark"
        return mode if mode in ("light", "dark") else "dark"

    def is_light(self):
        return self.current_mode() == "light"

    def projected_writes(self, mode, palette=None):
        if palette is None:
            palette = self.resolve(self.selection())
        session = (palette.get("session") or {}).get(mode) or {}
        writes = {
            self.state / "active" / name: Path(source).read_bytes()
            for name, source in session.items()
        }
        writes[self.state / "mode"] = f"{mode}\n".encode()
        return writes

    def write_projected(self, mode, writes=None, palette=None):
        if writes is None:
            writes = self.projected_writes(mode, palette)
        previous = {
            path: path.read_bytes() if path.exists() else None
            for path in writes
        }
        changed = []
        try:
            for path, content in writes.items():
                if previous[path] != content or path.is_symlink():
                    atomic_write(path, content)
                    changed.append(path)
        except OSError:
            for path in reversed(changed):
                if previous[path] is None:
                    path.unlink(missing_ok=True)
                else:
                    atomic_write(path, previous[path])
            raise

    def set_mode(self, mode):
        if mode not in ("light", "dark"):
            raise ValueError(f"Unknown appearance mode: {mode}")
        # Resolve the bundle before touching ThemeMode so a missing catalogue
        # cannot leave the greeter mode half-applied.
        projected = self.projected_writes(mode)
        content = b"false\n" if mode == "light" else b"true\n"
        previous = (
            self.mode_file.read_bytes() if self.mode_file.exists() else None
        )
        atomic_write(self.mode_file, content)
        try:
            self.write_projected(mode, projected)
        except OSError:
            if previous is None:
                self.mode_file.unlink(missing_ok=True)
            else:
                atomic_write(self.mode_file, previous)
            raise

    def publish_session(self):
        # Direct imports, including tests, must not touch the live desktop.
        if os.environ.get("THEME_MENU_PUBLISH") != "1":
            return []
        if not hyprland_session():
            return []
        warnings = []
        mode = self.current_mode()
        conf = self.state / "active" / "hyprland.conf"
        try:
            variables = theme_variables(conf.read_text())
        except FileNotFoundError:
            variables = {}
            warnings.append(
                "Hyprland theme file is missing. Run theme-menu --reapply."
            )
        warnings.extend(self._publish_gtk(mode, variables))
        warnings.extend(self._install_gtk_css())
        warnings.extend(self._reload_hyprland(variables))
        warnings.extend(self._reload_waybar())
        try:
            subprocess.run(
                ["makoctl", "reload"],
                capture_output=True, timeout=5, check=True
            )
        except (OSError, subprocess.SubprocessError):
            warnings.append(
                "Notifications will use the selected theme on next startup."
            )
        return warnings

    def _publish_gtk(self, mode, variables):
        scheme = variables.get(
            "theme_color_scheme",
            "prefer-dark" if mode == "dark" else "prefer-light",
        )
        gtk = variables.get(
            "theme_gtk", "adw-gtk3-dark" if mode == "dark" else "adw-gtk3"
        )
        font = variables.get("theme_font", "Ubuntu")
        mono = variables.get("theme_mono_font", "JetBrainsMono Nerd Font")
        size = variables.get("theme_font_size", "12")
        commands = [
            [
                "gsettings",
                "set",
                "org.gnome.desktop.interface",
                "color-scheme",
                scheme,
            ],
            [
                "gsettings",
                "set",
                "org.gnome.desktop.interface",
                "gtk-theme",
                gtk,
            ],
            [
                "gsettings",
                "set",
                "org.gnome.desktop.interface",
                "font-name",
                f"{font} {size}",
            ],
            [
                "gsettings",
                "set",
                "org.gnome.desktop.interface",
                "monospace-font-name",
                f"{mono} {size}",
            ],
        ]
        for command in commands:
            try:
                subprocess.run(
                    command,
                    capture_output=True,
                    text=True,
                    timeout=5,
                    check=True,
                )
            except (OSError, subprocess.SubprocessError) as error:
                detail = getattr(error, "stderr", "") or str(error)
                return [
                    "Could not publish org.gnome.desktop.interface. "
                    "Needs gsettings (glib), gsettings-desktop-schemas, "
                    "and dconf. "
                    + detail.strip()
                ]
        return []

    def _install_gtk_css(self):
        source = self.state / "active" / "gtk.css"
        if not source.exists():
            return []
        content = source.read_bytes()
        warnings = []
        for relative in GTK_CSS:
            dest = self.config / relative
            if dest.is_symlink():
                # COSMIC exports these links. Replace the link, never its
                # target, when Hyprland takes over GTK appearance. Preserve
                # unrelated user/Home Manager overrides.
                cosmic_css = self.config / "gtk-4.0/cosmic"
                if dest.resolve() not in (
                    cosmic_css / "dark.css", cosmic_css / "light.css"
                ):
                    continue
            elif (
                dest.exists()
                and b"Shared GTK and Brave colours and fonts"
                not in dest.read_bytes()
            ):
                warnings.append(
                    f"Preserved custom {relative}; "
                    "it overrides desktop colours."
                )
                continue
            try:
                atomic_write(dest, content)
            except OSError as error:
                warnings.append(f"Could not install {relative}: {error}")
        return warnings

    def _reload_hyprland(self, variables):
        if shutil.which("hyprctl") is None:
            return [
                "hyprctl is not installed. Hyprland colours apply on the "
                "next reload. Package: hyprland."
            ]
        commands = []
        mapping = (
            ("theme_active_border", "general:col.active_border"),
            ("theme_inactive_border", "general:col.inactive_border"),
            ("theme_background", "misc:background_color"),
            ("theme_rounding", "decoration:rounding"),
            ("theme_border_size", "general:border_size"),
        )
        for key, keyword in mapping:
            value = variables.get(key)
            if value:
                commands.append(["hyprctl", "keyword", keyword, value])
        for command in commands:
            try:
                subprocess.run(
                    command,
                    capture_output=True,
                    text=True,
                    timeout=5,
                    check=True,
                )
            except (OSError, subprocess.SubprocessError) as error:
                detail = getattr(error, "stderr", "") or str(error)
                return [
                    "hyprctl could not apply theme colours. "
                    + detail.strip()
                ]
        return []

    def _reload_waybar(self):
        # Do not scan /proc. That signals every Waybar on the machine.
        if shutil.which("systemctl") is None:
            return [
                "systemctl is not installed, so Waybar was not reloaded. "
                "Package: systemd."
            ]
        try:
            subprocess.run(
                [
                    "systemctl",
                    "--user",
                    "kill",
                    "--signal=SIGUSR2",
                    WAYBAR_UNIT,
                ],
                capture_output=True,
                text=True,
                timeout=5,
                check=True,
            )
        except (OSError, subprocess.SubprocessError) as error:
            detail = getattr(error, "stderr", "") or str(error)
            return [
                f"Could not signal user unit {WAYBAR_UNIT}. "
                "The Hyprland module should run Waybar as that unit. "
                + detail.strip()
            ]
        return []

    def launcher_style(self):
        appearance = self.catalogue.get("appearance") or {}
        font = appearance.get("monoFont", "JetBrainsMono Nerd Font")
        size = appearance.get("fontSize", 12)
        border = appearance.get("borderSize", 2)
        radius = appearance.get("rounding", 12)
        font_spec = f"{font}:size={size}"
        try:
            session = (
                self.resolve(self.selection()).get("session") or {}
            ).get(self.current_mode()) or {}
            source = session.get("fuzzel.ini")
        except (OSError, ValueError, KeyError):
            source = None
        if not source:
            return font_spec, border, radius
        section = ""
        values = {}
        for line in Path(source).read_text().splitlines():
            stripped = line.strip()
            if stripped.startswith("[") and stripped.endswith("]"):
                section = stripped[1:-1]
                continue
            if "=" not in stripped or stripped.startswith("#"):
                continue
            key, value = stripped.split("=", 1)
            values[(section, key.strip())] = value.strip()
        return (
            values.get(("main", "font"), font_spec),
            values.get(("border", "width"), border),
            values.get(("border", "radius"), radius),
        )

    def choose(self):
        current = self.selection()
        light = self.is_light()
        target_mode = "dark" if light else "light"
        palettes = self.catalogue["palettes"]
        keys = [
            target_mode,
            "default",
            *sorted(palettes, key=lambda key: palettes[key]["label"]),
        ]
        labels = [
            f"Switch to {target_mode} mode",
            f"Host default ({self.resolve('default')['label']})",
            *[palettes[key]["label"] for key in keys[2:]],
        ]
        rows = [
            f"{'*' if key == current else ' '} {label}"
            for key, label in zip(keys, labels)
        ]
        colours = (
            self.resolve(current)
            .get("nvim", {})
            .get("light" if light else "dark", {})
        )

        def colour(key, fallback):
            return colours.get(key, fallback).lstrip("#") + "ff"

        # Keep styling in an INI file, like voice-menu. The pinned Fuzzel's
        # --font argument also triggers an invalid free with --check-config.
        font_spec, border_width, radius = self.launcher_style()
        launcher = self.catalogue.get("launcher", {})
        settings = f"""[main]
font={font_spec}
anchor={launcher.get('anchor', 'center')}
layer={launcher.get('layer', 'overlay')}
width={launcher.get('width', 40)}
lines={min(len(rows), launcher.get('lines', len(rows)))}
minimal-lines=yes
match-mode={launcher.get('matchMode', 'fzf')}
icons-enabled=no
horizontal-pad=20
vertical-pad=12
inner-pad=8
[colors]
background={colour("base00", "24273a")}
text={colour("base05", "cad3f5")}
input={colour("base05", "cad3f5")}
prompt={colour("base0D", "b7bdf8")}
selection={colour("base02", "494d64")}
selection-text={colour("base05", "cad3f5")}
selection-match={colour("base0D", "b7bdf8")}
match={colour("base0D", "b7bdf8")}
border={colour("base0D", "b7bdf8")}
[border]
width={border_width}
radius={radius}
"""
        with tempfile.TemporaryDirectory(prefix="theme-menu-") as directory:
            config = Path(directory) / "fuzzel.ini"
            config.write_text(settings)
            result = subprocess.run(
                [
                    "fuzzel",
                    "--config",
                    str(config),
                    "--dmenu",
                    "--index",
                    "--only-match",
                    "--prompt",
                    "Theme > ",
                    "--select-index",
                    str(keys.index(current)),
                ],
                input="\n".join(rows) + "\n",
                capture_output=True,
                text=True,
            )
        if result.returncode == 1:  # Escape, or no selection.
            return None
        if result.returncode != 0:
            raise RuntimeError(f"Fuzzel failed: {result.stderr.strip()}")
        value = result.stdout.strip()
        if not value.isdecimal() or int(value) >= len(keys):
            raise ValueError("Invalid theme menu selection")
        return keys[int(value)]


def reload_apps():
    warnings = []
    try:
        owner = subprocess.run(
            [
                "gdbus",
                "call",
                "--session",
                "--dest",
                "org.freedesktop.DBus",
                "--object-path",
                "/org/freedesktop/DBus",
                "--method",
                "org.freedesktop.DBus.NameHasOwner",
                "com.mitchellh.ghostty",
            ],
            capture_output=True,
            text=True,
            timeout=5,
            check=True,
        )
        if "true" in owner.stdout:
            subprocess.run(
                [
                    "gdbus",
                    "call",
                    "--session",
                    "--dest",
                    "com.mitchellh.ghostty",
                    "--object-path",
                    "/com/mitchellh/ghostty",
                    "--method",
                    "org.gtk.Actions.Activate",
                    "reload-config",
                    "[]",
                    "{}",
                ],
                capture_output=True,
                text=True,
                timeout=5,
                check=True,
            )
    except (OSError, subprocess.SubprocessError):
        warnings.append(
            "Reload Ghostty configuration manually (Ctrl+Shift+,)."
        )
    try:
        subprocess.run(
            ["herdr", "server", "reload-config"],
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
    except (OSError, subprocess.SubprocessError):
        warnings.append(
            "Herdr was not reloaded. "
            "If running, use herdr server reload-config."
        )
    return warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalogue", type=Path, required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument(
        "--reapply",
        action="store_true",
        help="Restore selection after Home Manager activation",
    )
    parser.add_argument("--no-reload", action="store_true")
    parser.add_argument(
        "palette",
        nargs="?",
        help="light, dark, default or a palette ID; omit for the popup",
    )
    args = parser.parse_args()
    try:
        controller = Themes(
            json.loads(args.catalogue.read_text()),
            args.state,
            Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")),
        )
        args.state.mkdir(parents=True, exist_ok=True)
        with (args.state / "lock").open("w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError(
                    "Another theme menu is open; close it and retry."
                ) from None
            default_mode = controller.catalogue.get("defaultMode")
            if args.reapply and default_mode is not None:
                controller.set_mode(default_mode)
            selected = controller.selection() if args.reapply else args.palette
            if selected is None:
                selected = controller.choose()
            if selected is None:
                return 0
            if selected in ("light", "dark"):
                controller.set_mode(selected)
                title, label = "Appearance mode", f"{selected.title()} mode"
                # COSMIC and terminals propagate mode changes natively. Do not
                # rewrite the palette or disturb explicit application overrides.
                # Hyprland session assets follow the selected mode separately.
                warnings = []
            else:
                controller.apply(selected)
                title, label = (
                    "Theme selected",
                    controller.resolve(selected)["label"],
                )
                warnings = [] if args.no_reload else reload_apps()
            if not args.no_reload:
                warnings.extend(controller.publish_session())
        for warning in warnings:
            print(f"theme-menu: {warning}", file=sys.stderr)
        if not args.reapply and not args.no_reload:
            subprocess.run(
                [
                    "notify-send",
                    title,
                    label + ("\n" + "\n".join(warnings) if warnings else ""),
                ],
                check=False,
                timeout=5,
            )
        return 0
    except (
        OSError,
        ValueError,
        RuntimeError,
        subprocess.SubprocessError,
    ) as error:
        print(f"theme-menu: {error}", file=sys.stderr)
        if not args.reapply and not args.no_reload:
            try:
                subprocess.run(
                    ["notify-send", "Theme selection failed", str(error)],
                    check=False,
                    timeout=5,
                )
            except (OSError, subprocess.SubprocessError):
                pass
        return 1


if __name__ == "__main__":
    sys.exit(main())
