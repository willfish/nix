"""Read-only passage retrieval from a memory-mapped ANN generation."""

import bisect
import json
import os
from pathlib import Path
from threading import Lock
import zlib

os.environ.setdefault("OMP_NUM_THREADS", "2")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "2")
os.environ.setdefault("RAYON_NUM_THREADS", "2")
import faiss
import numpy as np
from embeddings import (
    MODEL,
    MODEL_REVISION,
    OVERLAP,
    WINDOW,
    load_model,
    unpack_id,
)
from library import metadata, search_papers


class SemanticSearch:
    def __init__(self, root):
        self.root = Path(root)
        self.model = None
        self.index = None
        self.generation = None
        self.lock = Lock()
        faiss.omp_set_num_threads(2)

    def state(self):
        path = self.root / "embeddings" / "index.json"
        if not path.exists():
            return None
        return json.loads(path.read_text())

    def load(self, db):
        state = self.state()
        if not state:
            raise ValueError(
                "Embedding index is not published yet; use bm25 search"
            )
        config = state["config"]
        revision = db.execute(
            "SELECT value FROM settings WHERE key='revision'"
        ).fetchone()[0]
        if (
            config["model"],
            config["model_revision"],
            config["window_chars"],
            config["overlap_chars"],
            config["dataset_revision"],
        ) != (MODEL, MODEL_REVISION, WINDOW, OVERLAP, revision):
            raise ValueError(
                "Embedding generation does not match this library/model/chunker"
            )
        if self.generation != state["path"]:
            if self.model is None:
                self.model = load_model(self.root)
            path = self.root / "embeddings" / state["path"]
            if (
                path.parent != self.root / "embeddings"
                or path.stat().st_size != state["size"]
            ):
                raise ValueError("Invalid ANN snapshot file")
            index = faiss.read_index(
                str(path), faiss.IO_FLAG_MMAP | faiss.IO_FLAG_READ_ONLY
            )
            if index.ntotal != state["passages"] or index.d != self.model.dim:
                raise ValueError("ANN snapshot shape mismatch")
            if state["kind"] == "ivf-pq":
                index.nprobe = min(index.nlist, 64)
            self.index = index
            self.generation = state["path"]
        return state

    def search(self, db, query, limit=10, category=None):
        # FastMCP may dispatch concurrent calls. Keep the mutable encoder/index
        # and its block-routing snapshot consistent throughout each request.
        with self.lock:
            return self._search(db, query, limit, category)

    def _search(self, db, query, limit=10, category=None):
        if (
            not isinstance(query, str)
            or not query.strip()
            or len(query) > 1000
        ):
            raise ValueError("query must contain 1..1000 characters")
        if not isinstance(limit, int) or not 1 <= limit <= 50:
            raise ValueError("limit must be 1..50")
        state = self.load(db)
        vector = self.model.encode(
            [query], max_length=None, use_multiprocessing=False
        )
        faiss.normalize_L2(vector)
        scores, packed_ids = self.index.search(
            vector, min(state["passages"], max(200, limit * 40))
        )
        # PQ scores are approximate. Re-rank candidates using the original
        # normalized passage vectors before deduplicating papers.
        reranked = self.rerank(vector[0], packed_ids[0], state)
        seen, results = set(), []
        for score, packed in reranked:
            if packed < 0:
                continue
            row_id, passage = unpack_id(packed)
            if row_id in seen:
                continue
            seen.add(row_id)
            row = db.execute(
                "SELECT * FROM papers WHERE id=?", (row_id,)
            ).fetchone()
            if row is None:
                raise ValueError("Index references a missing paper")
            if category and row["category"] != category:
                continue
            text = zlib.decompress(row["body"]).decode("utf-8")
            start = passage * (WINDOW - OVERLAP)
            if start > len(text):
                raise ValueError("Index references an invalid paper offset")
            results.append(
                metadata(row)
                | {
                    "semantic_score": float(score),
                    "excerpt": text[start : start + 800],
                    "excerpt_offset": start,
                    "passage_end": min(len(text), start + WINDOW),
                }
            )
            if len(results) == limit:
                break
        return {
            "papers": results,
            "indexed_papers": state["papers"],
            "indexed_passages": state["passages"],
            "model": MODEL,
            "category_filter": (
                "post-filtered ANN candidates" if category else None
            ),
        }

    def rerank(self, query, packed_ids, state):
        blocks = state.get("blocks")
        if blocks is None:
            # Compatibility with the initial unpublished-format prototype.
            paths = sorted((self.root / "embeddings").glob("block-*"))
            blocks = [
                [int(p.name.split("-")[1]), p.name]
                for p in paths
                if int(p.name.split("-")[1]) <= state["last_id"]
            ]
        ends = [b[0] for b in blocks]
        groups = {}
        for packed in packed_ids:
            if packed < 0:
                continue
            row_id, _ = unpack_id(packed)
            block = bisect.bisect_left(ends, row_id)
            if block >= len(blocks):
                raise ValueError("ANN references an unknown embedding block")
            groups.setdefault(block, []).append(int(packed))
        results = []
        for block, candidates in groups.items():
            path = self.root / "embeddings" / blocks[block][1]
            ids = np.load(path / "ids.npy", mmap_mode="r", allow_pickle=False)
            vectors = np.load(
                path / "vectors.npy", mmap_mode="r", allow_pickle=False
            )
            positions = np.searchsorted(ids, candidates)
            if np.any(positions >= len(ids)) or not np.array_equal(
                ids[positions], candidates
            ):
                raise ValueError("ANN references a missing passage vector")
            values = np.asarray(vectors[positions], dtype=np.float32)
            faiss.normalize_L2(values)
            scores = values @ query
            results.extend(
                (float(score), packed)
                for score, packed in zip(scores, candidates)
            )
        return sorted(results, key=lambda pair: (-pair[0], pair[1]))


def hybrid_search(db, semantic, query, limit=10, category=None):
    if not isinstance(limit, int) or not 1 <= limit <= 50:
        raise ValueError("limit must be 1..50")
    lexical = search_papers(db, query, 50, category)
    dense = semantic.search(db, query, 50, category)
    scores, papers = {}, {}
    for results in (lexical, dense["papers"]):
        for rank, result in enumerate(results, start=1):
            pid = result["paper_id"]
            scores[pid] = scores.get(pid, 0) + 1 / (60 + rank)
            papers[pid] = papers.get(pid, {}) | result
    ordered = sorted(papers, key=lambda pid: (-scores[pid], pid))[:limit]
    return dense | {
        "papers": [
            papers[pid] | {"rrf_score": scores[pid]} for pid in ordered
        ],
        "fusion": "reciprocal rank fusion k=60",
    }
