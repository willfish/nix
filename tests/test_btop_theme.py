"""btop follows the selected Omarchy palette, including shipped theme files."""

import importlib.util
import json
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "theme_menu", ROOT / "home/config/appearance/theme_menu.py"
)
menu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(menu)

TEMPLATE_KEYS = (
    "main_bg",
    "main_fg",
    "title",
    "hi_fg",
    "selected_bg",
    "selected_fg",
    "inactive_fg",
    "graph_text",
    "meter_bg",
    "proc_misc",
    "cpu_box",
    "mem_box",
    "net_box",
    "proc_box",
    "div_line",
    "temp_start",
    "temp_mid",
    "temp_end",
    "cpu_start",
    "cpu_mid",
    "cpu_end",
    "free_start",
    "free_mid",
    "free_end",
    "cached_start",
    "cached_mid",
    "cached_end",
    "available_start",
    "available_mid",
    "available_end",
    "used_start",
    "used_mid",
    "used_end",
    "download_start",
    "download_mid",
    "download_end",
    "upload_start",
    "upload_mid",
    "upload_end",
    "process_start",
    "process_mid",
    "process_end",
    *(f"gradient_color_{index}" for index in range(8)),
)
SHIPPED = {"last-horizon", "lumon", "retro-82", "solitude"}


def theme_values(text):
    values = {}
    for line in text.splitlines():
        if not line.startswith("theme[") or "]=" not in line:
            continue
        key, value = line.split("]=", 1)
        values[key.removeprefix("theme[")] = value.strip().strip('"')
    return values


class BtopThemeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        expression = f"""
          let
            flake = builtins.getFlake {json.dumps(str(ROOT))};
            pkgs = flake.inputs.nixpkgs.legacyPackages.
              ${{builtins.currentSystem}};
            btopTheme = import {ROOT}/home/user/themes/btop.nix {{
              inherit (flake.inputs.nixpkgs) lib;
              inherit pkgs;
            }};
            palettes = import {ROOT}/home/user/themes/palettes.nix;
          in builtins.mapAttrs (name: theme:
            let
              mode = theme.nativeMode or "dark";
              file = btopTheme name theme.${{mode}};
            in {{
              inherit mode;
              text = builtins.readFile file;
              shipped = builtins.pathExists (
                (import {ROOT}/home/user/themes/omarchy-source.nix)
                + "/themes/${{name}}/btop.theme"
              );
            }}
          ) palettes
        """
        cls.rendered = json.loads(
            subprocess.check_output(
                ["nix", "eval", "--impure", "--json", "--expr", expression],
                text=True,
            )
        )

    def test_every_theme_has_a_btop_file(self):
        self.assertEqual(set(self.rendered), set(self.rendered))
        self.assertGreaterEqual(len(self.rendered), 22)
        self.assertEqual(set(self.rendered) & SHIPPED, SHIPPED)

    def test_generated_themes_use_shared_roles_and_distinct_blue(self):
        rose = theme_values(self.rendered["rose-pine"]["text"])
        self.assertEqual(rose["main_bg"], "#faf4ed")
        self.assertEqual(rose["main_fg"], "#575279")
        self.assertEqual(rose["cpu_box"], "#907aa9")
        self.assertEqual(rose["cpu_mid"], "#56949f")
        hackerman = theme_values(self.rendered["hackerman"]["text"])
        self.assertEqual(hackerman["proc_box"], "#82FB9C")
        self.assertEqual(hackerman["cpu_mid"], "#829dd4")
        self.assertNotEqual(hackerman["cpu_mid"], hackerman["proc_box"])
        for name, payload in self.rendered.items():
            if name in SHIPPED:
                continue
            values = theme_values(payload["text"])
            with self.subTest(theme=name):
                self.assertFalse(payload["shipped"])
                self.assertEqual(set(values), set(TEMPLATE_KEYS))
                for key in TEMPLATE_KEYS:
                    self.assertRegex(values[key], r"#[0-9a-fA-F]{6}")

    def test_shipped_themes_are_kept_verbatim(self):
        source = subprocess.check_output(
            [
                "nix-instantiate",
                "--eval",
                "--raw",
                "--expr",
                f"import {ROOT}/home/user/themes/omarchy-source.nix",
            ],
            text=True,
        )
        for name in SHIPPED:
            upstream = Path(source) / "themes" / name / "btop.theme"
            self.assertEqual(
                self.rendered[name]["text"], upstream.read_text()
            )

    def test_reload_signals_only_btop(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "12").mkdir()
            (root / "12" / "comm").write_text("btop\n")
            (root / "13").mkdir()
            (root / "13" / "comm").write_text("not-btop\n")
            (root / "note").mkdir()
            signaled = []

            def kill(pid, sig):
                signaled.append((pid, sig))

            original = menu.os.kill
            menu.os.kill = kill
            try:
                menu.reload_btop(root)
            finally:
                menu.os.kill = original
            self.assertEqual(signaled, [(12, signal.SIGUSR2)])

    def test_graphical_config_follows_the_published_theme(self):
        expression = """
          let
            flake = builtins.getFlake %s;
            pkgs = flake.inputs.nixpkgs.legacyPackages.
              ${builtins.currentSystem};
            home = flake.homeConfigurations."william@andromeda".config;
            terminus = flake.homeConfigurations."william@terminus".config;
            source = home.xdg.configFile."btop/themes/host.theme".source;
          in {
            color = home.programs.btop.settings.color_theme;
            save = home.programs.btop.settings.save_config_on_exit;
            background = home.programs.btop.settings.theme_background;
            force = home.xdg.configFile."btop/btop.conf".force;
            theme = builtins.readFile (
              pkgs.runCommand "btop-theme-link" {} ''
                readlink ${source} > "$out"
              ''
            );
            staticTheme = builtins.readFile
              terminus.xdg.configFile."btop/themes/host.theme".source;
          }
        """ % json.dumps(str(ROOT))
        config = json.loads(
            subprocess.check_output(
                ["nix", "eval", "--impure", "--json", "--expr", expression],
                text=True,
            )
        )
        self.assertEqual(config["color"], "host")
        self.assertFalse(config["save"])
        self.assertTrue(config["background"])
        self.assertTrue(config["force"])
        self.assertIn("/theme-menu/active/btop.theme", config["theme"])
        self.assertIn('theme[main_bg]="#1e1e2e"', config["staticTheme"])


if __name__ == "__main__":
    unittest.main()
