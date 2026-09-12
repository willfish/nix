"""Apply immutable palette bundles to writable, watched application files."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


COSMIC_NAMES = ("Light", "Dark", "Light.Builder", "Dark.Builder")


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
            relative = (
                Path("cosmic") / f"com.system76.CosmicTheme.{variant}" / "v1"
            )
            source_dir = cosmic / relative.relative_to("cosmic")
            sources = list(source_dir.iterdir())
            if not sources:
                raise ValueError(f"Incomplete COSMIC palette: {variant}")
            for source in sources:
                if source.is_file():
                    writes[self.config / relative / source.name] = (
                        source.read_bytes()
                    )
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

    def choose(self):
        current = self.selection()
        palettes = self.catalogue["palettes"]
        keys = [
            "default",
            *sorted(palettes, key=lambda key: palettes[key]["label"]),
        ]
        labels = [
            f"Host default ({self.resolve('default')['label']})",
            *[palettes[key]["label"] for key in keys[1:]],
        ]
        rows = [
            f"{'*' if key == current else ' '} {label}"
            for key, label in zip(keys, labels)
        ]
        mode_file = (
            self.config / "cosmic/com.system76.CosmicTheme.Mode/v1/is_dark"
        )
        light = mode_file.exists() and mode_file.read_text().strip() == "false"
        colours = (
            self.resolve(current)
            .get("nvim", {})
            .get("light" if light else "dark", {})
        )

        def colour(key, fallback):
            return colours.get(key, fallback).lstrip("#") + "ff"

        # Keep styling in an INI file, like voice-menu. The pinned Fuzzel's
        # --font argument also triggers an invalid free with --check-config.
        settings = f"""[main]
font=JetBrainsMono Nerd Font:size=12
anchor=center
layer=overlay
width=40
lines={len(rows)}
minimal-lines=yes
match-mode=fzf
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
width=2
radius=12
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
        help="default or a palette ID; omit for the popup",
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
            selected = controller.selection() if args.reapply else args.palette
            if selected is None:
                selected = controller.choose()
            if selected is None:
                return 0
            controller.apply(selected)
            warnings = [] if args.no_reload else reload_apps()
        for warning in warnings:
            print(f"theme-menu: {warning}", file=sys.stderr)
        if not args.reapply and not args.no_reload:
            subprocess.run(
                [
                    "notify-send",
                    "Theme selected",
                    controller.resolve(selected)["label"]
                    + ("\n" + "\n".join(warnings) if warnings else ""),
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
