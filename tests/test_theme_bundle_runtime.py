"""Validate generated bundles with the pinned applications."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "theme_menu", ROOT / "home/config/appearance/theme_menu.py"
)
menu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(menu)


class ThemeBundleRuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        paths = subprocess.check_output(
            [
                "nix",
                "build",
                "--no-link",
                "--print-out-paths",
                str(ROOT)
                + "#homeConfigurations.william-linux.config.xdg.configFile."
                '"theme-menu/catalogue.json".source',
                str(ROOT) + "#homeConfigurations.william-linux.pkgs.fuzzel",
                str(ROOT) + "#homeConfigurations.william-linux.pkgs.ghostty",
            ],
            text=True,
        ).splitlines()
        cls.catalogue = json.loads(Path(paths[0]).read_text())
        cls.fuzzel = str(Path(paths[1]) / "bin/fuzzel")
        cls.ghostty = str(Path(paths[2]) / "bin/ghostty")

    def test_all_bundles_apply_and_match_their_rendered_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            controller = menu.Themes(
                self.catalogue, root / "state", root / "config"
            )
            for key, palette in self.catalogue["palettes"].items():
                with self.subTest(palette=key):
                    controller.apply(key)
                    self.assertEqual(controller.selection(), key)
                    self.assertNotIn("cosmic", palette)
                    self.assertEqual(
                        (root / "state/mode").read_text().strip(),
                        palette["nativeMode"],
                    )
                    for name, source in palette["files"].items():
                        self.assertEqual(
                            (root / "state/active" / name).read_bytes(),
                            Path(source).read_bytes(),
                        )
                    herdr = tomllib.loads(
                        (root / "state/active/herdr.toml").read_text()
                    )
                    self.assertEqual(herdr["theme"], palette["herdrTheme"])
                    self.assertFalse((root / "config/cosmic").exists())
                    for mode_name in ("light", "dark"):
                        result = subprocess.run(
                            [
                                self.ghostty,
                                "+validate-config",
                                "--config-file="
                                + str(
                                    root
                                    / "state/active"
                                    / f"ghostty-{mode_name}"
                                ),
                            ],
                            capture_output=True,
                            text=True,
                            timeout=10,
                        )
                        self.assertEqual(result.returncode, 0, result.stderr)
            controller.apply("default")
            self.assertEqual(controller.selection(), "default")

    def test_real_fuzzel_accepts_every_palette_in_both_modes(self):
        run = subprocess.run
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            controller = menu.Themes(
                self.catalogue, root / "state", root / "config"
            )
            for key in self.catalogue["palettes"]:
                controller.apply(key)
                for mode_name in ("light", "dark"):
                    (root / "state/mode").write_text(mode_name + "\n")
                    with self.subTest(palette=key, mode=mode_name):

                        def check(args, **kwargs):
                            result = run(
                                [self.fuzzel, *args[1:], "--check-config"],
                                **kwargs,
                                timeout=10,
                            )
                            self.assertEqual(
                                result.returncode, 0, result.stderr
                            )
                            # Native themes put default first, without mode.
                            return subprocess.CompletedProcess(
                                args, 0, "0\n", ""
                            )

                        with patch.object(
                            menu.subprocess, "run", side_effect=check
                        ):
                            self.assertEqual(controller.choose(), "default")


if __name__ == "__main__":
    unittest.main()
