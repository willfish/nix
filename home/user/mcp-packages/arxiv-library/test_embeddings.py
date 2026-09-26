import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
from embeddings import encode_paper, generate, spans, unpack_id, vector_id
from library import add_paper, connect
from test_library import paper


class FakeModel:
    dim = 4

    def __init__(self):
        self.texts = []

    def encode(self, texts, **kwargs):
        assert (
            kwargs["max_length"] is None
        ), "must not truncate full-text passages"
        self.texts.extend(texts)
        return np.asarray(
            [[1, len(t) / 10000, 0, 0] for t in texts], dtype=np.float32
        )


class EmbeddingTest(unittest.TestCase):
    def test_spans_cover_every_character_with_bounded_overlap(self):
        for length in [0, 1, 8191, 8192, 8193, 16384, 100000]:
            text = "x" * length
            positions = list(spans(text))
            self.assertEqual(positions[0][0], 0)
            self.assertEqual(positions[-1][1], length)
            end = 0
            for a, b in positions:
                self.assertLessEqual(a, end)
                self.assertLessEqual(b - a, 8192)
                end = b
        with self.assertRaises(ValueError):
            list(spans("x", 10, 10))

    def test_int64_ids_round_trip(self):
        for row in [1, 2856227, 2**31 - 1]:
            for passage in [0, 1, 6500, 2**32 - 1]:
                packed = vector_id(row, passage)
                self.assertLessEqual(packed, np.iinfo(np.int64).max)
                self.assertEqual(unpack_id(packed), (row, passage))
        with self.assertRaises(ValueError):
            vector_id(2**31, 0)

    def test_resume_integrity_and_full_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "manifest.json").write_text(json.dumps({"files": []}))
            db = connect(root / "library.sqlite3", write=True)
            with db:
                db.execute("INSERT INTO settings VALUES ('revision','test')")
                for i in range(3):
                    add_paper(
                        db,
                        paper(
                            str(i),
                            text="start " + "λ data " * 3000 + "ENDOFPAPER",
                        ),
                    )
            model = FakeModel()
            generate(root, model, block_papers=2, max_blocks=1)
            self.assertEqual(
                len(list((root / "embeddings").glob("block-*"))), 1
            )
            self.assertTrue(any("ENDOFPAPER" in t for t in model.texts))
            self.assertTrue(any("λ" in t for t in model.texts))
            generate(root, model, block_papers=2)
            blocks = sorted((root / "embeddings").glob("block-*"))
            self.assertEqual(len(blocks), 2)
            all_ids = np.concatenate([np.load(b / "ids.npy") for b in blocks])
            self.assertEqual(set(unpack_id(x)[0] for x in all_ids), {1, 2, 3})
            self.assertEqual(len(all_ids), len(set(all_ids)))
            for b in blocks:
                matrix = np.load(b / "vectors.npy")
                self.assertTrue(np.isfinite(matrix).all())
                np.testing.assert_allclose(
                    np.linalg.norm(matrix.astype(np.float32), axis=1),
                    1,
                    atol=0.001,
                )
            count = len(model.texts)
            generate(root, model)
            self.assertEqual(len(model.texts), count)
            (blocks[0] / "vectors.npy").write_bytes(b"corrupt")
            with self.assertRaises(ValueError):
                generate(root, model)
            db.close()


if __name__ == "__main__":
    unittest.main()
