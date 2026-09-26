"""Initialize storage and acquire only hash-pinned public model artifacts."""

import json
from pathlib import Path
import sys

from embeddings import MODEL, MODEL_REVISION, load_model, write_json


def prepare(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    source = json.loads(Path(__file__).with_name("manifest.json").read_text())
    manifest = root / "manifest.json"
    if manifest.exists():
        if json.loads(manifest.read_text()) != source:
            raise ValueError(
                "Existing corpus manifest differs; refusing to replace it"
            )
    else:
        pending = root / "manifest.pending"
        write_json(pending, source)
        pending.replace(manifest)
    model_dir = root / "models" / "potion-science-32M"
    lock = json.loads(Path(__file__).with_name("model-lock.json").read_text())
    if not all((model_dir / item["path"]).exists() for item in lock["files"]):
        from huggingface_hub import snapshot_download

        snapshot_download(
            MODEL,
            revision=MODEL_REVISION,
            token=False,
            allow_patterns=[
                "config.json",
                "model.safetensors",
                "tokenizer.json",
                "README.md",
            ],
            local_dir=str(model_dir),
        )
    load_model(root)


if __name__ == "__main__":
    prepare(sys.argv[1])
