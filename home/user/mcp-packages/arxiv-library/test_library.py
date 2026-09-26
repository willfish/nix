import hashlib
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

from library import add_paper, connect, get_paper, search_papers, status
from ingest import ingest


def paper(
    pid="test/001", text="rarephysicalterm and quantum dynamics λ " * 30
):
    return {
        "paper_id": pid,
        "title": "A paper",
        "abstract": "An abstract",
        "text": text,
        "primary_category": "physics",
        "license": "test",
        "text_sha256": hashlib.sha256(text.encode()).hexdigest(),
    }


class LibraryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "library.sqlite3"
        self.db = connect(self.path, write=True)

    def tearDown(self):
        self.db.close()
        self.tmp.cleanup()

    def test_full_body_search_and_exact_pagination(self):
        row = paper(text="preamble " * 10000 + "rarephysicalterm λ at the end")
        with self.db:
            add_paper(self.db, row)
        result = search_papers(self.db, "rarephysicalterm")[0]
        self.assertEqual(result["paper_id"], row["paper_id"])
        self.assertIn("rarephysicalterm", result["excerpt"])
        text, offset = "", 0
        while offset is not None:
            part = get_paper(self.db, row["paper_id"], offset, 777)
            text += part["text"]
            offset = part["next_offset"]
        self.assertEqual(text, row["text"])
        self.assertEqual(
            get_paper(self.db, row["paper_id"], len(text) + 10)["text"], ""
        )

    def test_filters_and_duplicate_integrity(self):
        with self.db:
            self.assertTrue(add_paper(self.db, paper()))
            self.assertFalse(add_paper(self.db, paper()))
        self.assertEqual(status(self.db)["papers"], 1)
        self.assertEqual(
            len(search_papers(self.db, "quantum", category="physics")), 1
        )
        self.assertEqual(
            search_papers(self.db, "quantum", category="math"), []
        )
        with self.assertRaises(ValueError):
            add_paper(self.db, paper(text="changed"))
        wrong = paper("wrong")
        wrong["text_sha256"] = "bad"
        with self.assertRaises(ValueError):
            add_paper(self.db, wrong)

    def test_ranking_fetches_only_selected_bodies(self):
        with self.db:
            for i in range(30):
                add_paper(self.db, paper(str(i)))
        statements = []
        self.db.set_trace_callback(statements.append)
        results = search_papers(self.db, "quantum", limit=2)
        self.db.set_trace_callback(None)
        self.assertEqual([r["paper_id"] for r in results], ["0", "1"])
        body_reads = [
            sql
            for sql in statements
            if sql.startswith("SELECT * FROM papers WHERE id=")
        ]
        self.assertEqual(len(body_reads), 2)
        self.assertFalse(any("SELECT p.*" in sql for sql in statements))

    def test_inputs_missing_readonly_and_rollback(self):
        for query in ("", "*", "x " * 33, "a" * 1001):
            with self.assertRaises(ValueError):
                search_papers(self.db, query)
        with self.assertRaises(ValueError):
            get_paper(self.db, "missing", -1)
        with self.assertRaises(KeyError):
            get_paper(self.db, "missing")
        with self.assertRaises(RuntimeError):
            with self.db:
                add_paper(self.db, paper())
                raise RuntimeError("interrupted transaction")
        self.assertEqual(status(self.db)["papers"], 0)
        self.assertEqual(search_papers(self.db, "quantum"), [])
        ro = connect(self.path)
        with self.assertRaises(sqlite3.OperationalError):
            ro.execute("DELETE FROM papers")
        ro.close()


class IngestTest(unittest.TestCase):
    def test_verified_resume_and_idempotence(self):
        import pyarrow as pa
        import pyarrow.parquet as pq

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shard = root / "paper_text/test.parquet"
            shard.parent.mkdir()
            rows = [paper(str(i)) for i in range(17)]
            pq.write_table(pa.Table.from_pylist(rows), shard, row_group_size=5)
            item = {
                "path": "paper_text/test.parquet",
                "size": shard.stat().st_size,
                "lfs": {"oid": hashlib.sha256(shard.read_bytes()).hexdigest()},
            }
            (root / "manifest.json").write_text(
                json.dumps({"revision": "test", "files": [item]})
            )
            ingest(root, max_papers=7)
            db = connect(root / "library.sqlite3")
            self.assertEqual(status(db)["papers"], 7)
            self.assertEqual(status(db)["imports"][0]["rows_done"], 7)
            db.close()
            ingest(root)
            ingest(root)
            db = connect(root / "library.sqlite3")
            self.assertEqual(status(db)["papers"], 17)
            self.assertEqual(status(db)["imports"][0]["complete"], 1)
            self.assertEqual(len(search_papers(db, "quantum", limit=50)), 17)
            db.close()

    def test_invalid_shard_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "test.parquet").write_bytes(b"broken")
            item = {"path": "test.parquet", "size": 6, "lfs": {"oid": "bad"}}
            (root / "manifest.json").write_text(
                json.dumps({"revision": "test", "files": [item]})
            )
            with self.assertRaises(ValueError):
                ingest(root)


if __name__ == "__main__":
    unittest.main()
