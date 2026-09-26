"""Publish immutable, memory-mappable FAISS indexes from passage blocks."""

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import re
import time

import faiss
import numpy as np
from embeddings import file_hash, write_json


def normalized(values):
    values = np.array(values, dtype=np.float32, copy=True, order="C")
    faiss.normalize_L2(values)
    return values


def prune_snapshots(folder, current, keep=2, min_age=86400):
    """Remove obsolete generated indexes; retain a day for slow readers."""
    snapshots = sorted(
        p
        for p in Path(folder).glob("index-*.faiss")
        if re.fullmatch(r"index-\d{10}\.faiss", p.name) and not p.is_symlink()
    )
    protected = {p.name for p in snapshots[-keep:]} | {current}
    removed = []
    for path in snapshots:
        if (
            path.name not in protected
            and time.time() - path.stat().st_mtime >= min_age
        ):
            path.unlink()
            removed.append(path.name)
    return removed


def build(root, sample_size=200000):
    root = Path(root)
    folder = root / "embeddings"
    with (folder / "index.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        config = json.loads((folder / "config.json").read_text())
        blocks = sorted(folder.glob("block-*"))
        if not blocks:
            raise ValueError("No committed embeddings")
        states = [
            json.loads((b / "metadata.json").read_text()) for b in blocks
        ]
        last = 0
        for block, state in zip(blocks, states):
            if state["after_id"] != last:
                raise ValueError("Noncontiguous embedding blocks")
            for name, sha in state["sha256"].items():
                if file_hash(block / name) != sha:
                    raise ValueError(f"Corrupt embedding block: {block}")
            last = state["last_id"]
        pointer = folder / "index.json"
        if pointer.exists():
            old = json.loads(pointer.read_text())
            if (
                old["last_id"] == last
                and old["config"] == config
                and "blocks" in old
            ):
                prune_snapshots(folder, old["path"])
                return old
        total = sum(s["passages"] for s in states)
        dimension = config["dimension"]
        if dimension % 4:
            raise ValueError("Embedding dimension must be divisible by four")
        faiss.omp_set_num_threads(2)
        # Uniform deterministic sample over the complete current generation,
        # rather than training a permanent codebook on the first paper batch.
        rng = np.random.default_rng(42)
        selected = np.sort(
            rng.choice(total, min(total, sample_size), replace=False)
        )
        samples = []
        offset = 0
        for block, state in zip(blocks, states):
            positions = (
                selected[
                    (selected >= offset)
                    & (selected < offset + state["passages"])
                ]
                - offset
            )
            if len(positions):
                data = np.load(
                    block / "vectors.npy", mmap_mode="r", allow_pickle=False
                )
                samples.append(data[positions])
            offset += state["passages"]
        training = normalized(np.concatenate(samples))
        started = time.monotonic()
        if total < 10000:
            index = faiss.IndexIDMap2(faiss.IndexFlatIP(dimension))
            kind = "flat"
        else:
            nlist = 2 ** int(math.log2(max(1, min(4096, len(training) // 50))))
            index = faiss.IndexIVFPQ(
                faiss.IndexFlatIP(dimension),
                dimension,
                nlist,
                dimension // 4,
                8,
                faiss.METRIC_INNER_PRODUCT,
            )
            index.cp.seed = 42
            index.pq.cp.seed = 42
            index.train(training)
            index.nprobe = min(nlist, 64)
            kind = "ivf-pq"
        for block in blocks:
            vectors = np.load(
                block / "vectors.npy", mmap_mode="r", allow_pickle=False
            )
            ids = np.load(block / "ids.npy", mmap_mode="r", allow_pickle=False)
            if vectors.shape != (len(ids), dimension):
                raise ValueError(f"Invalid vector block shape: {block}")
            # Limit copies during addition; the compressed index grows at 72
            # bytes/passage for the 256-dimensional scientific model.
            for start in range(0, len(ids), 16384):
                index.add_with_ids(
                    normalized(vectors[start : start + 16384]),
                    np.asarray(ids[start : start + 16384]),
                )
        name = f"index-{last:010d}.faiss"
        pending = folder / (name + ".pending")
        faiss.write_index(index, str(pending))
        with pending.open("rb") as f:
            os.fsync(f.fileno())
        result = {
            "path": name,
            "kind": kind,
            "last_id": last,
            "papers": sum(s["papers"] for s in states),
            "passages": total,
            "config": config,
            "size": pending.stat().st_size,
            "sha256": file_hash(pending),
            "seconds": round(time.monotonic() - started, 2),
            "blocks": [
                [state["last_id"], block.name]
                for block, state in zip(blocks, states)
            ],
        }
        os.replace(pending, folder / name)
        write_json(folder / "index.pending", result)
        os.replace(folder / "index.pending", pointer)
        fd = os.open(folder, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        prune_snapshots(folder, name)
        print(json.dumps(result), flush=True)
        return result


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("root", type=Path)
    args = p.parse_args()
    build(args.root)
