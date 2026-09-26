"""Resume verified Parquet imports into a full-text SQLite library."""

import argparse
from contextlib import ExitStack, closing
import fcntl
import json
from pathlib import Path
import time

from download import digest
from library import add_paper, connect


def ingest(root, max_papers=None, watch=False):
    import pyarrow.parquet as pq

    root = Path(root)
    with (root / "ingest.lock").open("w") as lock, ExitStack() as resources:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest = json.loads((root / "manifest.json").read_text())
        db = resources.enter_context(
            closing(connect(root / "library.sqlite3", write=True))
        )
        prior = db.execute(
            "SELECT value FROM settings WHERE key='revision'"
        ).fetchone()
        if prior and prior[0] != manifest["revision"]:
            raise ValueError("Dataset revision changed; migration required")
        with db:
            db.execute(
                "INSERT OR IGNORE INTO settings VALUES ('revision',?)",
                (manifest["revision"],),
            )
            db.execute("INSERT OR IGNORE INTO settings VALUES ('schema','1')")
        processed = 0
        while True:
            incomplete = False
            for item in manifest["files"]:
                path = root / item["path"]
                checkpoint = db.execute(
                    "SELECT * FROM imports WHERE shard=?", (item["path"],)
                ).fetchone()
                if checkpoint and checkpoint["complete"]:
                    continue
                if not path.exists():
                    incomplete = True
                    continue
                if (
                    path.stat().st_size != item["size"]
                    or digest(path) != item["lfs"]["oid"]
                ):
                    raise ValueError(f"Invalid shard: {path}")
                start = checkpoint["rows_done"] if checkpoint else 0
                parquet = pq.ParquetFile(path)
                done = start
                group_start = 0
                print(
                    f"Import {path.name} from row "
                    f"{start}/{parquet.metadata.num_rows}",
                    flush=True,
                )
                for group in range(parquet.num_row_groups):
                    group_rows = parquet.metadata.row_group(group).num_rows
                    if group_start + group_rows <= start:
                        group_start += group_rows
                        continue
                    row_offset = group_start
                    for batch in parquet.iter_batches(
                        batch_size=32, row_groups=[group]
                    ):
                        rows = batch.to_pylist()
                        with db:
                            for row in rows:
                                if row_offset >= start:
                                    if (
                                        max_papers is not None
                                        and processed >= max_papers
                                    ):
                                        break
                                    add_paper(db, row)
                                    processed += 1
                                    done = row_offset + 1
                                row_offset += 1
                            db.execute(
                                "INSERT INTO imports(shard,rows_done,complete) "
                                "VALUES (?,?,0) ON CONFLICT(shard) DO UPDATE "
                                "SET rows_done=excluded.rows_done",
                                (item["path"], done),
                            )
                        if max_papers is not None and processed >= max_papers:
                            print(
                                f"Limit reached, checkpoint {done}", flush=True
                            )
                            db.close()
                            return
                    group_start += group_rows
                    print(
                        f"{path.name}: {done}/{parquet.metadata.num_rows}",
                        flush=True,
                    )
                with db:
                    db.execute(
                        "UPDATE imports SET complete=1 WHERE shard=?",
                        (item["path"],),
                    )
                db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
                print(f"Finished {path.name}", flush=True)
            if not watch or not incomplete:
                break
            print("Waiting for remaining verified downloads", flush=True)
            time.sleep(60)
        db.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    parser.add_argument("--max-papers", type=int)
    parser.add_argument("--watch", action="store_true")
    args = parser.parse_args()
    ingest(args.root, args.max_papers, args.watch)
