import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

from download import main


class DownloadTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.data = b"complete parquet stand-in"
        item = {
            "path": "paper_text/test.parquet",
            "size": len(self.data),
            "lfs": {"oid": hashlib.sha256(self.data).hexdigest()},
        }
        (self.root / "manifest.json").write_text(
            json.dumps({"revision": "pinned", "files": [item]})
        )
        self.target = self.root / item["path"]
        self.target.parent.mkdir()
        self.partial = self.target.with_suffix(".partial")

    def tearDown(self):
        self.tmp.cleanup()

    def test_download_verified_atomic_and_idempotent(self):
        def fetch(args):
            self.assertIn("--continue-at", args)
            self.assertIn("/resolve/pinned/", args[-1])
            Path(args[args.index("--output") + 1]).write_bytes(self.data)
            self.assertFalse(self.target.exists())
            return SimpleNamespace(returncode=0)

        with patch("download.subprocess.run", side_effect=fetch) as run:
            main(self.root)
            main(self.root)
            self.assertEqual(run.call_count, 1)
        self.assertEqual(self.target.read_bytes(), self.data)
        self.assertFalse(self.partial.exists())

    def test_completed_partial_recovers_without_range_request(self):
        self.partial.write_bytes(self.data)
        with patch("download.subprocess.run") as run:
            main(self.root)
            run.assert_not_called()
        self.assertEqual(self.target.read_bytes(), self.data)

    def test_corrupt_files_fail_closed(self):
        self.target.write_bytes(b"x" * len(self.data))
        with patch("download.subprocess.run") as run:
            with self.assertRaises(RuntimeError):
                main(self.root)
            run.assert_not_called()
        self.target.unlink()
        self.partial.write_bytes(b"x" * len(self.data))
        with self.assertRaises(RuntimeError):
            main(self.root)
        self.assertFalse(self.target.exists())

    def test_retry_keeps_partial(self):
        calls = 0

        def fetch(args):
            nonlocal calls
            calls += 1
            if calls == 1:
                self.partial.write_bytes(self.data[:5])
                return SimpleNamespace(returncode=1)
            self.assertEqual(self.partial.read_bytes(), self.data[:5])
            self.partial.write_bytes(self.data)
            return SimpleNamespace(returncode=0)

        with patch("download.subprocess.run", side_effect=fetch), patch(
            "download.time.sleep"
        ):
            main(self.root)
        self.assertEqual(calls, 2)
        self.assertEqual(self.target.read_bytes(), self.data)


if __name__ == "__main__":
    unittest.main()
