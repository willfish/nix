"""Numerical and CLI contract for the narrow design colour-pair helper."""

import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest


SCRIPT = (Path(__file__).resolve().parents[1] /
          "home/config/llm/skills/design-workflow/scripts/contrast.py")
spec = importlib.util.spec_from_file_location("design_contrast", SCRIPT)
contrast = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contrast)


class DesignContrastTests(unittest.TestCase):
    def cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *args],
                              text=True, capture_output=True, check=False)

    def test_black_white_and_identical(self):
        self.assertEqual(contrast.ratio("#000000", "#ffffff"), 21)
        self.assertEqual(contrast.ratio("#ffffff", "#000000"), 21)
        self.assertEqual(contrast.ratio("#18212b", "#18212b"), 1)
        self.assertEqual(contrast.luminance("#FFFFFF"), 1)
        self.assertEqual(contrast.luminance("#000000"), 0)

    def test_channel_weighting_and_transfer_boundary(self):
        self.assertAlmostEqual(contrast.luminance("#ff0000"), 0.2126)
        self.assertAlmostEqual(contrast.luminance("#00ff00"), 0.7152)
        self.assertAlmostEqual(contrast.luminance("#0000ff"), 0.0722)
        self.assertAlmostEqual(
            contrast.luminance("#0a0a0a"), (10 / 255) / 12.92)
        self.assertAlmostEqual(contrast.luminance("#0b0b0b"),
                               ((11 / 255 + 0.055) / 1.055) ** 2.4)

    def test_near_threshold_does_not_round_to_pass(self):
        self.assertLess(contrast.ratio("#777777", "#ffffff"), 4.5)
        self.assertGreater(contrast.ratio("#767676", "#ffffff"), 4.5)
        self.assertEqual(self.cli("#777777", "#ffffff").returncode, 1)
        self.assertEqual(self.cli("#767676", "#ffffff").returncode, 0)
        value = contrast.ratio("#777777", "#ffffff")
        self.assertEqual(self.cli("#777777", "#ffffff", "--minimum",
                                 str(value + 0.00000001)).returncode, 1)

    def test_cli_output_and_explicit_non_text_threshold(self):
        result = self.cli("#777777", "#ffffff", "--minimum", "3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS", result.stdout)
        self.assertIn("opaque sRGB pair only", result.stdout)
        self.assertNotIn("compliant", result.stdout)
        self.assertEqual(
            self.cli("#000000", "#ffffff", "--minimum", "21").returncode, 0)
        self.assertEqual(
            self.cli("#ffffff", "#ffffff", "--minimum", "1").returncode, 0)

    def test_rejects_unknown_composition_and_bad_arguments(self):
        for colour in ("white", "#fff", "#12345678", "#zz0000", "123456",
                       " #000000"):
            with self.subTest(colour=colour):
                with self.assertRaises(ValueError):
                    contrast.luminance(colour)
                self.assertEqual(self.cli(colour, "#ffffff").returncode, 2)
        for minimum in ("0", "22", "nan", "inf", "-inf", "banana"):
            with self.subTest(minimum=minimum):
                self.assertEqual(self.cli("#000000", "#ffffff", "--minimum",
                                         minimum).returncode, 2)
        self.assertEqual(self.cli().returncode, 2)


if __name__ == "__main__":
    unittest.main()
