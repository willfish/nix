from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import resource
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from audit import audit, passage_count
from build_index import build
from embeddings import file_hash, generate, spans
from ingest import ingest
from library import connect
from test_library import paper
from test_semantic import ConceptModel


class AuditTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        shard = self.root / "papers.parquet"
        pq.write_table(
            pa.Table.from_pylist(
                [paper("a", "dogs " * 4000), paper("b", "galaxies")]
            ),
            shard,
        )
        item = {
            "path": shard.name,
            "size": shard.stat().st_size,
            "lfs": {"oid": hashlib.sha256(shard.read_bytes()).hexdigest()},
        }
        self.manifest = {"revision": "test", "files": [item]}
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))
        ingest(self.root)
        generate(self.root, ConceptModel())
        build(self.root)

    def tearDown(self):
        self.tmp.cleanup()

    def full(self, text_only=False):
        with patch("audit.load_model", return_value=ConceptModel()):
            return audit(
                self.root,
                full=True,
                expected_manifest=self.manifest,
                text_only=text_only,
            )

    def test_text_only_does_not_require_or_parse_embeddings(self):
        folder = self.root / "embeddings"
        retained = self.root / "retained-embeddings"
        folder.rename(retained)
        for missing in [True, False]:
            if not missing:
                retained.rename(folder)
                (folder / "index.json").write_text("unreadable old index")
                next(folder.glob("block-*/metadata.json")).write_text("bad")
            with self.subTest(missing=missing), patch(
                "audit.load_model", side_effect=AssertionError("not needed")
            ):
                quick = audit(
                    self.root, expected_manifest=self.manifest, text_only=True
                )
                self.assertTrue(quick["ready_for_full_audit"])
                self.assertFalse(quick["verified_complete"])
                result = audit(
                    self.root,
                    full=True,
                    expected_manifest=self.manifest,
                    text_only=True,
                )
                self.assertTrue(result["verified_complete"])
                self.assertEqual(result["scope"], "text")
                self.assertNotIn("encoded_papers", result)
                self.assertEqual(len(result["checks"]), 3)

    def test_missing_fts_rows_are_detected_in_both_modes(self):
        db = connect(self.root / "library.sqlite3", write=True)
        with db:
            db.execute("INSERT INTO search(search) VALUES('delete-all')")
        db.close()
        for text_only in [False, True]:
            with self.subTest(text_only=text_only):
                with self.assertRaisesRegex(ValueError, "FTS paper IDs"):
                    self.full(text_only=text_only)

    def test_full_proof_and_quick_progress_are_distinct(self):
        quick = audit(self.root, expected_manifest=self.manifest)
        self.assertTrue(quick["ready_for_full_audit"])
        self.assertFalse(quick["verified_complete"])
        self.assertTrue(self.full()["verified_complete"])
        for size in [0, 1, 8191, 8192, 8193, 16384, 50000]:
            self.assertEqual(passage_count(size), len(list(spans("x" * size))))

    def test_unpinned_manifest_is_not_proof(self):
        for text_only in [False, True]:
            result = audit(self.root, full=True, text_only=text_only)
            self.assertFalse(result["verified_complete"])
            self.assertIn(
                "Corpus manifest differs from the shipped source lock",
                result["reasons"],
            )

    def test_incomplete_import_is_not_proof(self):
        db = connect(self.root / "library.sqlite3", write=True)
        with db:
            db.execute("UPDATE imports SET complete=0")
        db.close()
        for text_only in [False, True]:
            result = self.full(text_only=text_only)
            self.assertFalse(result["verified_complete"])
            self.assertFalse(result["ready_for_full_audit"])

    def test_corrupted_body_and_source_are_detected(self):
        db = connect(self.root / "library.sqlite3", write=True)
        with db:
            db.execute("UPDATE papers SET text_chars=text_chars+1 WHERE id=1")
        db.close()
        for text_only in [False, True]:
            with self.assertRaisesRegex(ValueError, "Stored body"):
                self.full(text_only=text_only)
        path = self.root / "papers.parquet"
        data = bytearray(path.read_bytes())
        data[100] ^= 1
        path.write_bytes(data)
        for text_only in [False, True]:
            with self.assertRaisesRegex(ValueError, "Source checksum"):
                self.full(text_only=text_only)

    def test_published_block_routing_is_verified(self):
        path = self.root / "embeddings/index.json"
        state = json.loads(path.read_text())
        state["blocks"][0][1] = "block-wrong"
        path.write_text(json.dumps(state))
        with self.assertRaisesRegex(ValueError, "block routing"):
            self.full()

    def test_forged_vector_manifest_cannot_hide_duplicate_passage(self):
        block = next((self.root / "embeddings").glob("block-*"))
        ids = np.load(block / "ids.npy")
        ids[1] = ids[0]
        np.save(block / "ids.npy", ids)
        path = block / "metadata.json"
        state = json.loads(path.read_text())
        state["sha256"]["ids.npy"] = file_hash(block / "ids.npy")
        path.write_text(json.dumps(state))
        with self.assertRaisesRegex(ValueError, "Missing, duplicated"):
            self.full()


class AuditResourceTest(unittest.TestCase):
    def test_more_blocks_than_available_file_descriptors(self):
        with tempfile.TemporaryDirectory() as tmp, redirect_stdout(
            io.StringIO()
        ):
            root = Path(tmp)
            shard = root / "papers.parquet"
            pq.write_table(
                pa.Table.from_pylist(
                    [paper(str(i), "dogs") for i in range(80)]
                ),
                shard,
            )
            manifest = {
                "revision": "test",
                "files": [
                    {
                        "path": shard.name,
                        "size": shard.stat().st_size,
                        "lfs": {"oid": file_hash(shard)},
                    }
                ],
            }
            (root / "manifest.json").write_text(json.dumps(manifest))
            ingest(root)
            generate(root, ConceptModel(), block_papers=1)
            build(root)
            previous = resource.getrlimit(resource.RLIMIT_NOFILE)
            try:
                resource.setrlimit(resource.RLIMIT_NOFILE, (32, previous[1]))
                with patch("audit.load_model", return_value=ConceptModel()):
                    result = audit(root, full=True, expected_manifest=manifest)
                self.assertTrue(result["verified_complete"])
            finally:
                resource.setrlimit(resource.RLIMIT_NOFILE, previous)


if __name__ == "__main__":
    unittest.main()
