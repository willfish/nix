"""Incomplete target scans must never authorize staging."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = (
    Path(__file__).resolve().parents[1]
    / (
        "home/config/llm/skills/audiobook-library-import/scripts/"
        "source_target_duplicate_check.py"
    )
)
SPEC = importlib.util.spec_from_file_location("duplicate_preflight", SCRIPT)
duplicate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = duplicate
SPEC.loader.exec_module(duplicate)


class DuplicatePreflightTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.sources = self.root / "sources.txt"
        self.sources.write_text("Fixture Book\n")
        self.library = self.root / "library"
        self.library.mkdir()

    def run_check(self, roots, format="json", extra=()):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = duplicate.main(
                [
                    "--sources-file",
                    str(self.sources),
                    "--targets",
                    *map(str, roots),
                    "--format",
                    format,
                    *extra,
                ]
            )
        return status, output.getvalue()

    def test_missing_root_fails_with_structured_error(self):
        missing = self.root / "offline-library"
        status, output = self.run_check([self.library, missing])
        self.assertNotEqual(status, 0)
        report = json.loads(output)
        self.assertEqual(report["status"], "incomplete")
        self.assertIn(str(missing), str(report["scan_errors"]))

    def test_incomplete_markdown_never_marks_source_eligible(self):
        status, output = self.run_check([self.root / "missing"], "markdown")
        self.assertNotEqual(status, 0)
        self.assertIn("incomplete", output.lower())
        self.assertNotIn("eligible to stage", output.lower())

    def test_non_directory_root_is_an_error(self):
        status, output = self.run_check([self.sources])
        self.assertNotEqual(status, 0)
        self.assertEqual(json.loads(output)["status"], "incomplete")

    def test_report_lists_each_unavailable_root(self):
        roots = [self.root / "missing-one", self.root / "missing-two"]
        status, output = self.run_check(roots)
        self.assertNotEqual(status, 0)
        report = json.loads(output)
        self.assertEqual(
            {error["root"] for error in report["scan_errors"]},
            set(map(str, roots)),
        )
        self.assertIsNone(report["match_count"])

    def test_scan_honors_configured_depth(self):
        book = self.library / "author" / "Fixture Book"
        book.mkdir(parents=True)
        status, output = self.run_check(
            [self.library], extra=["--max-depth", "1"]
        )
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output)["match_count"], 0)
        self.assertEqual(json.loads(output)["max_depth"], 1)
        status, output = self.run_check(
            [self.library], extra=["--max-depth", "2"]
        )
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output)["match_count"], 1)

    def test_directory_read_failure_cannot_report_a_clean_scan(self):
        subtree = self.library / "author"
        subtree.mkdir()
        import os

        scandir = os.scandir

        def fail_subtree(path):
            if Path(path) == subtree:
                raise PermissionError("fixture directory unreadable")
            return scandir(path)

        with patch.object(os, "scandir", side_effect=fail_subtree):
            status, output = self.run_check([self.library])
        self.assertNotEqual(status, 0)
        self.assertEqual(json.loads(output)["status"], "incomplete")

    def test_empty_accessible_root_is_a_complete_scan(self):
        status, output = self.run_check([self.library])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output)["status"], "complete")
        self.assertEqual(json.loads(output)["match_count"], 0)

    def test_successful_scan_keeps_existing_matching_behavior(self):
        (self.library / "Fixture Book").mkdir()
        status, output = self.run_check([self.library])
        self.assertEqual(status, 0)
        self.assertEqual(
            json.loads(output)["matches"][0]["match_type"], "exact_name"
        )

    def test_incomplete_output_file_is_written_but_exit_is_nonzero(self):
        target = self.root / "report.json"
        status, stdout = self.run_check(
            [self.root / "missing"], extra=["-o", str(target)]
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(stdout, "")
        self.assertEqual(json.loads(target.read_text())["status"], "incomplete")


if __name__ == "__main__":
    unittest.main()
