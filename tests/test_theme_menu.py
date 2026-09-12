"""Runtime palette selection without a desktop or live user configuration."""

import importlib.util
import json
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
            for variant in menu.COSMIC_NAMES:
                cosmic = (
                    bundle / f"cosmic/com.system76.CosmicTheme.{variant}/v1"
                )
                cosmic.mkdir(parents=True)
                (cosmic / "background").write_text(name)
            self.palettes[name] = {
                "label": name,
                "cosmic": str(bundle),
                "files": {
                    "host-palettes.json": str(bundle / "host-palettes.json")
                },
            }
        self.catalogue = {"default": "tokyo-night", "palettes": self.palettes}
        self.controller = menu.Themes(self.catalogue, self.state, self.config)

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

    def test_mode_and_unrelated_cosmic_settings_are_preserved(self):
        mode = self.config / "cosmic/com.system76.CosmicTheme.Mode/v1/is_dark"
        mode.parent.mkdir(parents=True)
        mode.write_text("false\n")
        self.controller.apply("rose-pine")
        self.assertEqual(mode.read_text(), "false\n")
        self.assertEqual(
            (
                self.config
                / "cosmic/com.system76.CosmicTheme.Dark/v1/background"
            ).read_text(),
            "rose-pine",
        )

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
            if path.name == "background" and not failed:
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

    def test_replaces_legacy_cosmic_link_without_writing_store_target(self):
        target = (
            self.config / "cosmic/com.system76.CosmicTheme.Dark/v1/background"
        )
        target.parent.mkdir(parents=True)
        old = self.root / "old-store-file"
        old.write_text("old")
        target.symlink_to(old)
        self.controller.apply("rose-pine")
        self.assertFalse(target.is_symlink())
        self.assertEqual(old.read_text(), "old")

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

    def test_missing_cosmic_directory_aborts_before_writes(self):
        directory = (
            Path(self.palettes["rose-pine"]["cosmic"])
            / "cosmic/com.system76.CosmicTheme.Light/v1"
        )
        (directory / "background").unlink()
        directory.rmdir()
        with self.assertRaises(FileNotFoundError):
            self.controller.apply("rose-pine")
        self.assertFalse((self.state / "active/host-palettes.json").exists())

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
            run.return_value.stdout = "0\n"
            self.assertEqual(self.controller.choose(), "default")
            self.assertIn("* rose-pine", run.call_args.kwargs["input"])
            self.assertIn(
                "Host default (tokyo-night)", run.call_args.kwargs["input"]
            )


if __name__ == "__main__":
    unittest.main()
