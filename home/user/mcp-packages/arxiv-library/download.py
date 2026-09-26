"""Download pinned paper-text shards, resuming partials and checking SHA-256."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(8 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def main(root):
    manifest = json.loads((root / "manifest.json").read_text())
    for item in manifest["files"]:
        target = root / item["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        expected = item["lfs"]["oid"]
        if target.exists():
            if (
                target.stat().st_size != item["size"]
                or digest(target) != expected
            ):
                raise RuntimeError(f"Existing shard corrupt: {target}")
            print(f"Verified {target.name}", flush=True)
            continue
        partial = target.with_suffix(".partial")
        # A process can stop after receiving the last byte but before rename.
        # curl -C - would request an unsatisfiable range and loop on HTTP 416.
        if partial.exists() and partial.stat().st_size == item["size"]:
            if digest(partial) != expected:
                raise RuntimeError(f"Download integrity failure: {partial}")
            os.replace(partial, target)
            print(
                f"Recovered complete {target.name}: SHA-256 verified",
                flush=True,
            )
            continue
        url = (
            "https://huggingface.co/datasets/secemp9/arxiv-complete/resolve/"
            f"{manifest['revision']}/{item['path']}"
        )
        for attempt in range(20):
            # Avoid expired signed CDN redirects when resuming requests.
            result = subprocess.run(
                [
                    "curl",
                    "--fail",
                    "--location",
                    "--silent",
                    "--show-error",
                    "--connect-timeout",
                    "30",
                    "--speed-limit",
                    "1024",
                    "--speed-time",
                    "120",
                    "--retry",
                    "5",
                    "--retry-delay",
                    "10",
                    "--continue-at",
                    "-",
                    "--output",
                    str(partial),
                    url + f"?download=true&t={int(time.time())}",
                ]
            )
            if result.returncode == 0:
                break
            time.sleep(min(300, 10 * (attempt + 1)))
        else:
            raise RuntimeError(f"Download retries exhausted: {target.name}")
        if (
            partial.stat().st_size != item["size"]
            or digest(partial) != expected
        ):
            raise RuntimeError(f"Download integrity failure: {partial}")
        os.replace(partial, target)
        print(
            f'Completed {target.name}: {item["size"]} bytes, SHA-256 verified',
            flush=True,
        )


if __name__ == "__main__":
    main(Path(sys.argv[1]))
