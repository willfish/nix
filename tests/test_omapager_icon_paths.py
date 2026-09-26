import os
import tempfile
import unittest
from pathlib import Path

from importlib.util import module_from_spec, spec_from_file_location

ROOT = Path(__file__).resolve().parents[1]
SPEC = spec_from_file_location(
    "icon_paths",
    ROOT / "home/config/hyprland/omapager/icon_paths.py",
)
icon_paths = module_from_spec(SPEC)
SPEC.loader.exec_module(icon_paths)


class IconPathTest(unittest.TestCase):
    def test_regular_file_in_the_icon_dir_is_allowed(self):
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw)
            icon = base / "slack.png"
            icon.write_bytes(b"png")
            self.assertTrue(icon_paths.allowed_icon(str(icon), [str(base)]))

    def test_symlink_escape_is_rejected(self):
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw) / "icons"
            outside = Path(raw) / "secret"
            base.mkdir()
            outside.write_text("nope")
            link = base / "slack.png"
            link.symlink_to(outside)
            self.assertFalse(icon_paths.allowed_icon(str(link), [str(base)]))

    def test_nix_store_symlink_under_the_icon_dir_is_allowed(self):
        store = next(Path("/nix/store").glob("*-GitHub-Mark.png"), None)
        if store is None or not store.is_file():
            self.skipTest("GitHub mark is not in the store")
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw)
            link = base / "github.png"
            link.symlink_to(store)
            self.assertTrue(icon_paths.allowed_icon(str(link), [str(base)]))


if __name__ == "__main__":
    unittest.main()
