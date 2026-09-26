import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from prepare import prepare


class PrepareTest(unittest.TestCase):
    def test_initialize_and_reuse_pinned_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            def download(*args, **kwargs):
                self.assertFalse(kwargs["token"])
                self.assertEqual(
                    kwargs["revision"],
                    "7366079845507de14a4330007cdfa01bb92bca52",
                )
                folder = Path(kwargs["local_dir"])
                folder.mkdir(parents=True)
                for filename in kwargs["allow_patterns"]:
                    (folder / filename).write_text("fixture")

            with patch(
                "huggingface_hub.snapshot_download", side_effect=download
            ) as acquire, patch("prepare.load_model") as verify:
                prepare(root)
                manifest = (root / "manifest.json").read_bytes()
                prepare(root)
                self.assertEqual(acquire.call_count, 1)
                self.assertEqual(verify.call_count, 2)
                self.assertEqual(
                    (root / "manifest.json").read_bytes(), manifest
                )

    def test_do_not_replace_existing_corpus(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "manifest.json"
            path.write_text(json.dumps({"revision": "different"}))
            with self.assertRaises(ValueError):
                prepare(root)
            self.assertEqual(
                json.loads(path.read_text()), {"revision": "different"}
            )


if __name__ == "__main__":
    unittest.main()
