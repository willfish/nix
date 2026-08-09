"""Unit tests for source → target duplicate preflight helper."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).parents[1]
    / "home"
    / "config"
    / "llm"
    / "skills"
    / "audiobook-library-import"
    / "scripts"
    / "source_target_duplicate_check.py"
)


def load_script():
    spec = importlib.util.spec_from_file_location(
        "source_target_duplicate_check",
        SCRIPT_PATH,
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class SourceTargetDuplicateCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_script()

    def test_script_path_exists(self):
        self.assertTrue(
            SCRIPT_PATH.is_file(),
            f"missing shipped script {SCRIPT_PATH}",
        )

    def test_normalize_and_asin(self):
        mod = self.mod
        self.assertEqual(
            mod.normalize_title("The Electron (Unabridged)"),
            "the electron",
        )
        self.assertIn(
            "B0773WRVRD",
            mod.extract_asins("The Enigma of Reason [B0773WRVRD]"),
        )
        self.assertEqual(
            mod.normalize_title("Book Title [B0773WRVRD].m4b"),
            "book title",
        )

    def test_finds_exact_and_asin_matches(self):
        mod = self.mod
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "audiobooks"
            root.mkdir()
            hit = root / "The Electron"
            hit.mkdir()
            (hit / "audio.m4b").write_bytes(b"x" * 100)
            asin_dir = root / "Some Book [B0ABCDEF12]"
            asin_dir.mkdir()
            (asin_dir / "book.m4b").write_bytes(b"y" * 50)
            other = root / "Unrelated Title"
            other.mkdir()

            targets = mod.iter_target_entries([root], max_depth=2)
            sources = mod.load_sources(
                [
                    "The Electron",
                    "Whatever [B0ABCDEF12]",
                    "Brand New Lecture Series",
                ]
            )
            matches = mod.find_matches(sources, targets)
            types_by_source = {}
            for m in matches:
                types_by_source.setdefault(m.source, set()).add(m.match_type)

            self.assertIn("exact_name", types_by_source["The Electron"])
            self.assertIn("asin", types_by_source["Whatever [B0ABCDEF12]"])
            self.assertNotIn("Brand New Lecture Series", types_by_source)

            report = mod.format_markdown(sources, matches)
            self.assertIn("Source → target duplicate preflight", report)
            self.assertIn("Brand New Lecture Series", report)
            self.assertIn("No hit", report)

    def test_main_markdown(self):
        mod = self.mod
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            root = tmp_path / "lib"
            root.mkdir()
            book = root / "Sample Unabridged"
            book.mkdir()
            (book / "a.m4b").write_bytes(b"data")
            sources = tmp_path / "sources.txt"
            sources.write_text(
                "Sample Unabridged\nNew Book\n",
                encoding="utf-8",
            )
            out = tmp_path / "report.md"
            rc = mod.main(
                [
                    "--sources-file",
                    str(sources),
                    "--targets",
                    str(root),
                    "--format",
                    "markdown",
                    "-o",
                    str(out),
                ]
            )
            self.assertEqual(rc, 0)
            text = out.read_text(encoding="utf-8")
            self.assertIn("Sample Unabridged", text)
            self.assertIn("exact_name", text)
            self.assertIn("New Book", text)


if __name__ == "__main__":
    unittest.main()
