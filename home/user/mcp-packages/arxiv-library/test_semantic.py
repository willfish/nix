from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import tempfile
from threading import Barrier, Lock
import time
import unittest
from unittest.mock import patch

import numpy as np
from build_index import build
from embeddings import generate
from library import add_paper, connect
from semantic import SemanticSearch, hybrid_search
from test_library import paper


class ConceptModel:
    dim = 4

    def encode(self, texts, **kwargs):
        assert kwargs["max_length"] is None
        return np.array(
            [
                [
                    int("dogs" in t or "canines" in t),
                    int("galaxies" in t),
                    0.01,
                    0,
                ]
                for t in texts
            ],
            dtype=np.float32,
        )


class SemanticTest(unittest.TestCase):
    def test_real_faiss_publication_search_fusion_and_revision_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "manifest.json").write_text(json.dumps({"files": []}))
            db = connect(root / "library.sqlite3", write=True)
            with db:
                db.execute("INSERT INTO settings VALUES ('revision','test')")
                add_paper(db, paper("dog", "dogs are domesticated animals"))
                add_paper(db, paper("space", "galaxies contain stars"))
            model = ConceptModel()
            generate(root, model)
            info = build(root)
            self.assertEqual(info["passages"], 2)
            self.assertEqual(build(root), info)
            engine = SemanticSearch(root)
            engine.model = model
            result = engine.search(db, "canines")
            self.assertEqual(result["papers"][0]["paper_id"], "dog")
            self.assertEqual(result["papers"][0]["excerpt_offset"], 0)
            self.assertEqual(
                engine.search(db, "canines", category="nonexistent")["papers"],
                [],
            )
            result = hybrid_search(db, engine, "dogs", limit=1)
            self.assertEqual(result["papers"][0]["paper_id"], "dog")
            self.assertGreater(result["papers"][0]["rrf_score"], 1 / 61)
            with db:
                add_paper(db, paper("new-dog", "dogs and puppies"))
            generate(root, model)
            replacement = build(root)
            self.assertNotEqual(replacement["path"], info["path"])
            self.assertEqual(engine.search(db, "canines")["indexed_papers"], 3)
            self.assertEqual(engine.generation, replacement["path"])
            with db:
                db.execute(
                    "UPDATE settings SET value='other' WHERE key='revision'"
                )
            with self.assertRaises(ValueError):
                engine.search(db, "canines")
            db.close()

    def test_concurrent_requests_do_not_mix_generation_state(self):
        engine = SemanticSearch("unused")
        barrier, counter = Barrier(4), Lock()
        active, peak = 0, 0

        def search(db, query, limit, category):
            nonlocal active, peak
            with counter:
                active += 1
                peak = max(peak, active)
            time.sleep(0.02)
            with counter:
                active -= 1
            return query

        def request(number):
            barrier.wait(timeout=5)
            return engine.search(None, str(number))

        with patch.object(engine, "_search", side_effect=search):
            with ThreadPoolExecutor(max_workers=4) as pool:
                self.assertEqual(
                    list(pool.map(request, range(4))), ["0", "1", "2", "3"]
                )
        self.assertEqual(peak, 1)

    def test_not_ready_and_invalid_requests(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            db = connect(root / "library.sqlite3", write=True)
            engine = SemanticSearch(root)
            self.assertIsNone(engine.state())
            with self.assertRaises(ValueError):
                engine.search(db, "valid query")
            for query, limit in [
                ("", 1),
                ("x" * 1001, 1),
                ("x", 51),
                ("x", 0),
            ]:
                with self.assertRaises(ValueError):
                    engine.search(db, query, limit)
            db.close()


if __name__ == "__main__":
    unittest.main()
