import importlib.util
import json
import stat
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "wallpaper_cycle",
    ROOT / "home/config/hyprland/wallpaper_cycle.py",
)
cycle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cycle)


class NextWallpaperTest(unittest.TestCase):
    def test_single_image_does_not_advance(self):
        self.assertIsNone(cycle.next_name(["only.png"], "", "only.png"))

    def test_starts_after_the_preferred_image(self):
        names = ["a.png", "b.png", "c.png"]
        self.assertEqual(cycle.next_name(names, "", "b.png"), "c.png")

    def test_wraps(self):
        self.assertEqual(
            cycle.next_name(["a.png", "b.png"], "b.png", "a.png"), "a.png"
        )

    def test_rejects_a_path_segment(self):
        self.assertIsNone(
            cycle.next_name(["../secret.png", "a.png"], "", "a.png")
        )


class AdvanceTest(unittest.TestCase):
    def test_override_removes_a_rotation_root(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            state = root / "state"
            state.mkdir()
            link = state / "wallpaper-source"
            link.symlink_to(root)
            catalogue = root / "catalogue.json"
            catalogue.write_text(
                json.dumps(
                    {
                        "default": "rose",
                        "palettes": {
                            "rose": {
                                "session": {
                                    "dark": {
                                        "wallpaper.png": "/tmp/sky.png"
                                    }
                                }
                            }
                        },
                    }
                )
            )
            (state / "selection").write_text("rose\n")
            changed = cycle.advance(state, catalogue, str(root))
            self.assertFalse(changed)
            self.assertFalse(link.exists())

    def test_advance_converts_the_next_background(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            package = root / "theme"
            (package / "backgrounds").mkdir(parents=True)
            (package / "backgrounds" / "a.png").write_bytes(b"a")
            (package / "backgrounds" / "b.png").write_bytes(b"b")
            (package / "theme.json").write_text(
                json.dumps(
                    {
                        "backgrounds": ["a.png", "b.png"],
                        "preferred": "a.png",
                    }
                )
            )
            state = root / "state"
            (state / "active").mkdir(parents=True)
            (state / "selection").write_text("arch\n")
            (state / "mode").write_text("dark\n")
            catalogue = root / "catalogue.json"
            catalogue.write_text(
                json.dumps(
                    {
                        "default": "arch",
                        "palettes": {
                            "arch": {
                                "session": {
                                    "dark": {
                                        "wallpaper.png": "nix-theme:arch"
                                    }
                                }
                            }
                        },
                    }
                )
            )

            def run(args, check):
                if args[0] == "nix":
                    Path(args[3]).symlink_to(package)
                    return None
                expected = str(package / "backgrounds" / "b.png")
                self.assertEqual(args[1], expected)
                Path(args[2].removeprefix("PNG:")).write_bytes(b"png")
                return None

            with patch.object(cycle.subprocess, "run", side_effect=run):
                changed = cycle.advance(state, catalogue, str(root))
            self.assertTrue(changed)
            current = (state / "wallpaper-current").read_text()
            self.assertEqual(current, "b.png\n")
            self.assertEqual(
                (state / "active" / "wallpaper-live.png").read_bytes(), b"png"
            )
            self.assertEqual(
                stat.S_IMODE((state / "wallpaper-current").stat().st_mode),
                0o600,
            )
