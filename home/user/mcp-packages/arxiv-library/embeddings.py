"""Resumable full-text embeddings, independent of the ANN index format."""

import argparse
from contextlib import closing
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import time
import zlib

import numpy as np
from library import connect

MODEL = "minishlab/potion-science-32M"
MODEL_REVISION = "7366079845507de14a4330007cdfa01bb92bca52"
WINDOW = 8192
OVERLAP = 1024


def spans(text, size=WINDOW, overlap=OVERLAP):
    if size <= 0 or not 0 <= overlap < size:
        raise ValueError("Require 0 <= overlap < size")
    if not text:
        yield 0, 0
        return
    start = 0
    while start < len(text):
        end = min(len(text), start + size)
        yield start, end
        if end == len(text):
            break
        start = end - overlap


def vector_id(row_id, passage):
    if not 0 < row_id < 2**31 or not 0 <= passage < 2**32:
        raise ValueError("Paper or passage ID exceeds packed int64 limits")
    return (row_id << 32) | passage


def unpack_id(value):
    return int(value) >> 32, int(value) & (2**32 - 1)


def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for block in iter(lambda: f.read(8 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write_json(path, data):
    with path.open("w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())


def encode_paper(model, row):
    text = zlib.decompress(row["body"]).decode("utf-8")
    # Bound transient memory even for the 46 MB outlier paper.
    batch, ids = [], []
    for sequence, (start, end) in enumerate(spans(text)):
        batch.append(row["title"] + "\n" + text[start:end])
        ids.append(vector_id(row["id"], sequence))
        if len(batch) == 32:
            yield _encode(model, batch), np.asarray(ids, dtype=np.int64)
            batch, ids = [], []
    if batch:
        yield _encode(model, batch), np.asarray(ids, dtype=np.int64)


def _encode(model, texts):
    # model2vec's default is 512 tokens. Explicitly disable truncation so every
    # character in every passage contributes, including the ends of long papers.
    values = model.encode(
        texts, max_length=None, use_multiprocessing=False, batch_size=32
    )
    values = np.asarray(values, dtype=np.float32)
    if (
        values.shape != (len(texts), model.dim)
        or not np.isfinite(values).all()
    ):
        raise ValueError("Invalid embedding output")
    norms = np.linalg.norm(values, axis=1, keepdims=True)
    np.divide(values, norms, out=values, where=norms != 0)
    return values.astype(np.float16)


def load_model(root):
    from model2vec import StaticModel

    lock = json.loads(Path(__file__).with_name("model-lock.json").read_text())
    if lock["model"] != MODEL or lock["revision"] != MODEL_REVISION:
        raise ValueError("Unexpected model lock")
    folder = Path(root) / "models" / "potion-science-32M"
    for item in lock["files"]:
        path = folder / item["path"]
        if not path.exists() or path.stat().st_size != item["size"]:
            raise ValueError(
                f"Missing or wrong-sized pinned model file: {path}"
            )
        if "lfs" in item:
            valid = file_hash(path) == item["lfs"]["oid"]
        else:
            content = path.read_bytes()
            valid = (
                hashlib.sha1(
                    f"blob {len(content)}\0".encode() + content
                ).hexdigest()
                == item["oid"]
            )
        if not valid:
            raise ValueError(f"Model checksum mismatch: {path}")
    return StaticModel.from_pretrained(folder)


def generate(root, model, block_papers=1000, watch=False, max_blocks=None):
    if block_papers < 1 or (max_blocks is not None and max_blocks < 0):
        raise ValueError("Invalid generation limits")
    root = Path(root)
    output = root / "embeddings"
    output.mkdir(exist_ok=True)
    with (output / "writer.lock").open("w") as lock, closing(
        connect(root / "library.sqlite3")
    ) as db:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        dataset_revision = db.execute(
            "SELECT value FROM settings WHERE key='revision'"
        ).fetchone()[0]
        config = {
            "model": MODEL,
            "model_revision": MODEL_REVISION,
            "dimension": model.dim,
            "dataset_revision": dataset_revision,
            "window_chars": WINDOW,
            "overlap_chars": OVERLAP,
            "format": "float16-normalized",
            "max_tokens": None,
            "prefix": "title+newline",
            "schema": 1,
        }
        config_path = output / "config.json"
        if config_path.exists():
            if json.loads(config_path.read_text()) != config:
                raise ValueError(
                    "Embedding configuration changed; "
                    "use a separate generation directory"
                )
        else:
            write_json(output / "config.pending", config)
            os.replace(output / "config.pending", config_path)
        blocks = sorted(output.glob("block-*"))
        last = 0
        for block in blocks:
            state = json.loads((block / "metadata.json").read_text())
            if state["after_id"] != last:
                raise ValueError(
                    f"Noncontiguous embedding checkpoint: {block}"
                )
            for name, sha in state["sha256"].items():
                if file_hash(block / name) != sha:
                    raise ValueError(
                        f"Corrupt embedding block: {block / name}"
                    )
            last = state["last_id"]
        created = 0
        while max_blocks is None or created < max_blocks:
            # No long-lived read transaction, so WAL writers can checkpoint.
            ids = [
                r[0]
                for r in db.execute(
                    "SELECT id FROM papers WHERE id>? ORDER BY id LIMIT ?",
                    (last, block_papers),
                )
            ]
            if not ids:
                manifest = json.loads((root / "manifest.json").read_text())
                completed = db.execute(
                    "SELECT count(*) FROM imports WHERE complete=1"
                ).fetchone()[0]
                if not watch or completed == len(manifest["files"]):
                    break
                print(
                    f"Waiting for BM25 importer after paper row {last}",
                    flush=True,
                )
                time.sleep(60)
                continue
            pending = output / ".building"
            if pending.exists():
                shutil.rmtree(pending)
            pending.mkdir()
            vectors, vector_ids = [], []
            started = time.monotonic()
            characters = 0
            for row_id in ids:
                row = db.execute(
                    "SELECT id,title,body,text_chars FROM papers WHERE id=?",
                    (row_id,),
                ).fetchone()
                characters += row["text_chars"]
                for matrix, packed in encode_paper(model, row):
                    vectors.append(matrix)
                    vector_ids.append(packed)
            matrix = np.concatenate(vectors)
            packed = np.concatenate(vector_ids)
            for filename, array in [
                ("vectors.npy", matrix),
                ("ids.npy", packed),
            ]:
                with (pending / filename).open("wb") as f:
                    np.save(f, array, allow_pickle=False)
                    f.flush()
                    os.fsync(f.fileno())
            state = {
                "after_id": last,
                "last_id": ids[-1],
                "papers": len(ids),
                "passages": len(packed),
                "characters": characters,
                "sha256": {
                    name: file_hash(pending / name)
                    for name in ["vectors.npy", "ids.npy"]
                },
            }
            write_json(pending / "metadata.json", state)
            target = output / f"block-{ids[-1]:010d}"
            os.rename(pending, target)
            fd = os.open(output, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            last = ids[-1]
            created += 1
            print(
                json.dumps(
                    state | {"seconds": round(time.monotonic() - started, 2)}
                ),
                flush=True,
            )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("root", type=Path)
    p.add_argument("--watch", action="store_true")
    p.add_argument("--max-blocks", type=int)
    p.add_argument("--block-papers", type=int, default=1000)
    args = p.parse_args()
    model = load_model(args.root)
    generate(args.root, model, args.block_papers, args.watch, args.max_blocks)
