"""Runtime palette selection without a desktop or live user configuration."""

import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "theme_menu", ROOT / "home/config/appearance/theme_menu.py"
)
menu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(menu)


class ThemeMenuTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / "state"
        self.config = self.root / "config"
        self.palettes = {}
        for name in ("tokyo-night", "rose-pine"):
            bundle = self.root / name
            bundle.mkdir()
            (bundle / "host-palettes.json").write_text(
                json.dumps({"dark": {"base00": name}})
            )
            self.palettes[name] = {
                "label": name,
                "id": name,
                "files": {
                    "host-palettes.json": str(bundle / "host-palettes.json")
                },
            }
        self.catalogue = {"default": "tokyo-night", "palettes": self.palettes}
        self.greeter = self.root / "greeter-theme"
        self.controller = menu.Themes(
            self.catalogue, self.state, self.config, self.greeter
        )

    def test_default_override_reapply_and_reset(self):
        self.assertEqual(self.controller.selection(), "default")
        self.controller.apply("default")
        self.assertEqual(
            json.loads((self.state / "active/host-palettes.json").read_text())[
                "dark"
            ]["base00"],
            "tokyo-night",
        )
        self.controller.apply("rose-pine")
        self.assertEqual(self.controller.selection(), "rose-pine")
        # Reapplying preserves the override, not the old generation's bundle.
        Path(
            self.palettes["rose-pine"]["files"]["host-palettes.json"]
        ).write_text("new generation")
        self.controller.apply(self.controller.selection())
        self.assertEqual(
            (self.state / "active/host-palettes.json").read_text(),
            "new generation",
        )
        self.controller.apply("default")
        self.assertFalse((self.state / "selection").exists())

    def test_apply_preserves_state_mode(self):
        mode = self.state / "mode"
        mode.parent.mkdir(parents=True)
        mode.write_text("light\n")
        self.controller.apply("rose-pine")
        self.assertEqual(mode.read_text(), "light\n")

    def test_invalid_selection_and_missing_bundle_do_not_write(self):
        for invalid in ("../outside", "unknown"):
            with self.assertRaises(ValueError):
                self.controller.apply(invalid)
        Path(
            self.palettes["rose-pine"]["files"]["host-palettes.json"]
        ).unlink()
        with self.assertRaises(FileNotFoundError):
            self.controller.apply("rose-pine")
        self.assertFalse((self.state / "selection").exists())
        self.assertFalse((self.state / "active/host-palettes.json").exists())

    def test_failed_write_rolls_back_and_does_not_commit_selection(self):
        self.controller.apply("default")
        original = menu.atomic_write
        failed = False

        def fail_once(path, content):
            nonlocal failed
            if path.name == "host-palettes.json" and not failed:
                failed = True
                raise OSError("disk full")
            original(path, content)

        with patch.object(menu, "atomic_write", side_effect=fail_once):
            with self.assertRaises(OSError):
                self.controller.apply("rose-pine")
        self.assertEqual(self.controller.selection(), "default")
        self.assertIn(
            "tokyo-night",
            (self.state / "active/host-palettes.json").read_text(),
        )

    def test_preserves_gtk_links_without_writing_store_target(self):
        source = self.state / "active/gtk.css"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"/* Shared GTK and Brave colours and fonts */\n")
        store = self.root / "store-dark.css"
        store.write_text("store")
        destinations = [self.config / relative for relative in menu.GTK_CSS]
        for dest in destinations:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.symlink_to(store)
        self.assertEqual(self.controller._install_gtk_css(), [])
        for dest in destinations:
            self.assertTrue(dest.is_symlink())
        self.assertEqual(store.read_text(), "store")

    def test_popup_cancel_and_invalid_output_do_not_apply(self):
        for code, output in ((1, ""), (0, "999\n"), (0, "bad\n")):
            with (
                self.subTest(code=code, output=output),
                patch.object(menu.subprocess, "run") as run,
            ):
                run.return_value.returncode = code
                run.return_value.stdout = output
                if code == 1:
                    self.assertIsNone(self.controller.choose())
                else:
                    with self.assertRaises(ValueError):
                        self.controller.choose()
                self.assertFalse((self.state / "selection").exists())

    def test_mode_action_matches_current_setting_and_keeps_palette(self):
        self.controller.apply("rose-pine")
        before = (self.state / "active/host-palettes.json").read_bytes()
        for current, target in (
            (None, "light"),
            ("light", "dark"),
            ("dark", "light"),
        ):
            if current:
                self.controller.set_mode(current)
            with patch.object(menu.subprocess, "run") as run:
                run.return_value.returncode = 0
                run.return_value.stdout = "0\n"
                self.assertEqual(self.controller.choose(), target)
                self.assertEqual(
                    run.call_args.kwargs["input"].splitlines()[0],
                    f"  Switch to {target} mode",
                )
            self.controller.set_mode(target)
            self.assertEqual(self.controller.is_light(), target == "light")
            self.assertEqual(self.controller.selection(), "rose-pine")
            self.assertEqual(
                (self.state / "active/host-palettes.json").read_bytes(), before
            )

    def test_mode_action_is_explicit_even_if_settings_change_while_open(self):
        self.controller.set_mode("dark")
        with patch.object(menu.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "0\n"
            action = self.controller.choose()
        self.controller.set_mode("light")
        self.controller.set_mode(action)
        self.assertTrue(self.controller.is_light())

    def test_invalid_mode_and_failed_write_preserve_previous_mode(self):
        self.controller.set_mode("light")
        with self.assertRaises(ValueError):
            self.controller.set_mode("unknown")
        with patch.object(menu, "atomic_write", side_effect=OSError("full")):
            with self.assertRaises(OSError):
                self.controller.set_mode("dark")
        self.assertTrue(self.controller.is_light())

    def test_cli_mode_dispatch_notifies_without_reloading_or_changing_palette(
        self,
    ):
        self.controller.apply("rose-pine")
        catalogue = self.root / "catalogue.json"
        catalogue.write_text(json.dumps(self.catalogue))
        with (
            patch.object(
                menu.sys,
                "argv",
                [
                    "theme-menu",
                    "--catalogue",
                    str(catalogue),
                    "--state",
                    str(self.state),
                    "light",
                ],
            ),
            patch.dict(os.environ, {"XDG_CONFIG_HOME": str(self.config)}),
            patch.object(menu, "reload_apps") as reload_apps,
            patch.object(menu.subprocess, "run") as run,
        ):
            self.assertEqual(menu.main(), 0)
            reload_apps.assert_not_called()
            self.assertEqual(
                run.call_args.args[0],
                ["notify-send", "Appearance mode", "Light mode"],
            )
        self.assertTrue(self.controller.is_light())
        self.assertEqual(self.controller.selection(), "rose-pine")

    def test_native_mode_is_written_to_state_and_rejects_override(self):
        self.palettes["rose-pine"]["nativeMode"] = "light"
        self.controller.apply("rose-pine")
        self.assertEqual((self.state / "mode").read_text(), "light\n")
        with self.assertRaises(ValueError):
            self.controller.set_mode("dark")
        self.assertEqual((self.state / "mode").read_text(), "light\n")

    def test_reload_does_not_start_ghostty_and_reports_failures(self):
        with patch.object(menu.subprocess, "run") as run:
            run.return_value.stdout = "(false,)"
            self.assertEqual(menu.reload_apps(), [])
            self.assertEqual(run.call_count, 2)
            self.assertEqual(
                run.call_args_list[1].args[0],
                ["herdr", "server", "reload-config"],
            )
        with patch.object(
            menu.subprocess, "run", side_effect=OSError("not available")
        ):
            self.assertEqual(len(menu.reload_apps()), 2)

    def test_reload_uses_ghostty_public_action(self):
        with patch.object(menu.subprocess, "run") as run:
            run.return_value.stdout = "(true,)"
            self.assertEqual(menu.reload_apps(), [])
            self.assertEqual(run.call_count, 3)
            self.assertIn(
                "org.gtk.Actions.Activate", run.call_args_list[1].args[0]
            )
            self.assertIn("reload-config", run.call_args_list[1].args[0])

    def test_popup_errors_are_not_treated_as_cancellation(self):
        with patch.object(menu.subprocess, "run") as run:
            run.return_value.returncode = 2
            run.return_value.stderr = "invalid option"
            with self.assertRaisesRegex(RuntimeError, "invalid option"):
                self.controller.choose()

    def test_popup_maps_index_and_marks_current_selection(self):
        self.controller.apply("rose-pine")
        with patch.object(menu.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "1\n"
            self.assertEqual(self.controller.choose(), "default")
            self.assertIn("* rose-pine", run.call_args.kwargs["input"])
            self.assertIn(
                "Host default (tokyo-night)", run.call_args.kwargs["input"]
            )


if __name__ == "__main__":
    unittest.main()
