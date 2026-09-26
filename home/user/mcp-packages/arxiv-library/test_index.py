import json
import os
from pathlib import Path
import shutil
import tempfile
import time
import unittest

import faiss
import numpy as np

from audit import index_ids
from build_index import build, normalized, prune_snapshots
from embeddings import file_hash, vector_id
from semantic import SemanticSearch


class IndexTest(unittest.TestCase):
    def test_actual_ivfpq_mmap_reranking_ids_and_retention(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            folder = root / "embeddings"
            block = folder / "block-0000012000"
            block.mkdir(parents=True)
            rng = np.random.default_rng(7)
            values = normalized(
                rng.normal(size=(12000, 16)).astype(np.float32)
            ).astype(np.float16)
            ids = np.array(
                [vector_id(i + 1, 0) for i in range(len(values))],
                dtype=np.int64,
            )
            np.save(block / "vectors.npy", values)
            np.save(block / "ids.npy", ids)
            state = {
                "after_id": 0,
                "last_id": 12000,
                "papers": 12000,
                "passages": 12000,
                "sha256": {
                    name: file_hash(block / name)
                    for name in ["vectors.npy", "ids.npy"]
                },
            }
            (block / "metadata.json").write_text(json.dumps(state))
            (folder / "config.json").write_text(json.dumps({"dimension": 16}))
            info = build(root)
            self.assertEqual(info["kind"], "ivf-pq")
            index = faiss.read_index(
                str(folder / info["path"]),
                faiss.IO_FLAG_MMAP | faiss.IO_FLAG_READ_ONLY,
            )
            self.assertEqual(index.ntotal, len(ids))
            np.testing.assert_array_equal(np.sort(index_ids(index)), ids)
            index.nprobe = 64
            engine = SemanticSearch(root)
            queries = normalized(values[[3, 110, 720, 5020, 11999]])
            _, candidates = index.search(queries, 200)
            for expected, query, selected in zip(
                ids[[3, 110, 720, 5020, 11999]], queries, candidates
            ):
                ranked = engine.rerank(query, selected, info)
                self.assertEqual(ranked[0][1], expected)
                self.assertGreater(ranked[0][0], 0.999)
            # Existing mmap readers remain usable when an obsolete generation
            # is unlinked. The current pointer and newest two files survive.
            for number in [13000, 14000]:
                shutil.copyfile(
                    folder / info["path"],
                    folder / f"index-{number:010d}.faiss",
                )
            old_time = time.time() - 2 * 86400
            os.utime(folder / info["path"], (old_time, old_time))
            unrelated = folder / "index-manual.faiss"
            unrelated.write_text("not a generated snapshot")
            protected = prune_snapshots(folder, info["path"])
            self.assertEqual(protected, [])
            removed = prune_snapshots(folder, "index-0000014000.faiss")
            self.assertEqual(removed, [info["path"]])
            self.assertTrue(unrelated.exists())
            _, after = index.search(queries[:1], 200)
            self.assertIn(ids[3], after[0])

    def test_recent_snapshots_and_symlinks_are_not_pruned(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for n in range(4):
                (folder / f"index-{n:010d}.faiss").write_text("recent")
            (folder / "index-0000009999.faiss").symlink_to(
                folder / "index-0000000000.faiss"
            )
            self.assertEqual(
                prune_snapshots(folder, "index-0000000003.faiss"), []
            )
            self.assertTrue((folder / "index-0000009999.faiss").is_symlink())


if __name__ == "__main__":
    unittest.main()
