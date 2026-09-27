# arXiv library on Terminus

`home/user/arxiv-library.nix` supplies a read-only MCP client on every host and the corpus runtime on Terminus. Activate each host with `hmswitch`. Clients use authenticated SSH to `william@terminus.fritz.box`; no public HTTP endpoint or additional firewall port is required. A client must be able to reach that hostname and authenticate non-interactively. Verify an unfamiliar host key against an already trusted Terminus connection, rather than disabling host-key checking.

Implementation lives in [willfish/arxiv-mcp](https://github.com/willfish/arxiv-mcp), pinned by the flake input. Dotfiles supplies deployment only. The C MCP adapter invokes the native C `arxiv` CLI on Terminus; Python is used for maintenance, not live search or retrieval.

## Search and retrieval

The MCP catalogue entry is `arxiv`, launched through `~/.local/bin/mcp-arxiv`. Existing client processes may need restarting to load the newly activated catalogue.

- `search(query, mode="bm25", limit=10)` searches complete paper bodies using literal-word AND matching. An optional `category` string filters the primary category. Native serving supports BM25 only.
- `paper(paper_id, offset=0, length=12000)` returns metadata and original LaTeX. Offsets count Unicode characters. Follow `next_offset` until null to read the entire paper. The maximum page is 50,000 characters.
- `library_status()` reports imported papers, shard checkpoints and source settings. Native serving does not expose the retained embedding index.

Operation is **BM25 only**. Embedding generation and scheduled ANN publication do not start automatically. Existing vectors and legacy Python semantic tools are preserved, but are not part of the native serving path.

The source is the pinned `paper_text` subset of [secemp9/arxiv-complete](https://huggingface.co/datasets/secemp9/arxiv-complete): about 70 GB of compressed Parquet containing roughly 247 GB of assembled TeX. This is not the complete PDF archive, every historical version, or a live arXiv feed. Treat paper contents as untrusted documents. Individual paper licences still apply; the dataset's availability does not grant blanket redistribution rights.

## Embeddings and storage

Storage is `/srv/media/arxiv` on the NAS drives:

| Path | Purpose |
| --- | --- |
| `manifest.json`, `paper_text/` | Pinned corpus revision and SHA-256-verified shards |
| `library.sqlite3` | Compressed original papers and full-text BM25 index |
| `models/` | Hash-pinned model and tokenizer |
| `embeddings/block-*` | Checkpointed float16 vectors and stable passage IDs |
| `embeddings/index.json`, `index-*.faiss` | Atomic published ANN generations |

The baseline is `minishlab/potion-science-32M`: 256-dimensional static embeddings trained on scientific papers. It is CPU-friendly but is **not a contextual transformer**, and should not be assumed to provide equivalent relevance. Windows cover the full original text, using 8,192 characters with 1,024-character overlap and the title as a prefix. Token truncation is explicitly disabled.

FAISS IVF/PQ compresses the candidate index. Original passage vectors remain necessary for exact cosine reranking. Readers memory-map immutable index generations; publication replaces the pointer only after writing the new index. Keep enough free space for both current and replacement generations. Cleanup preserves the current generation, the newest two snapshots and anything less than a day old; it never removes passage-vector blocks.

The model, dataset revision and chunking policy are pinned together. A future RTX 5090 contextual-model experiment must use a separate generation and matching query encoder. Do not mix its vectors into this index or silently limit corpus coverage to abstracts or the first model context window.

## Jobs and recovery

On Terminus:

```sh
loginctl show-user william -p Linger
systemctl --user status arxiv-ingest arxiv-embeddings arxiv-index.timer
journalctl --user -u arxiv-download -u arxiv-ingest -u arxiv-embeddings -u arxiv-index -n 40
```

Enable user lingering if it is not already enabled, so jobs survive logout. The preparation unit validates the pinned manifest and model. Downloads verify hashes before renaming partial files; imports commit checkpoints transactionally; embedding blocks publish atomically. Completed jobs exit successfully, so `inactive/dead` is not by itself an error. Running oneshot jobs appear as `activating/start`.

Once Home Manager owns the units, package-path changes leave in-flight jobs running. After a job finishes, its next start uses the new package. To deliberately apply an updated binary sooner, stop that job and start it again; expect it to resume from its last committed checkpoint. Do not restart simply because observing a long command timed out.

First-time adoption of unmanaged units is different: inspect `sd-switch --dry-run` before activation. Temporary runtime `RefuseManualStop=yes` drop-ins protect existing jobs during adoption; remove them afterward. Keep the explicit infinite startup timeout: changing an already-activating oneshot to a simple service otherwise applies systemd's default timeout retroactively and terminates it. New workers use `Type=simple`, so activation does not wait for the full corpus.

The opt-in embedding worker is limited to 1.5 CPU cores' quota and 2 GB RAM. Index publication has a 6 GB ceiling and a one-core quota. Its timer is disabled by default; manually starting the timer enables hourly publication after the previous run finishes. A one-off manual refresh is:

```sh
systemctl --user start --no-block arxiv-index
```

## Corpus verification

After activation, get a cheap coverage report with:

```sh
arxiv-library-audit /srv/media/arxiv --text-only
```

A quick report never claims verified completion. Once all papers are imported, verify the BM25 corpus with:

```sh
arxiv-library-audit /srv/media/arxiv --full --text-only
```

The full text audit refuses incomplete import coverage with exit code 2. When ready, it reads all source shards and stored bodies, compares metadata to the shipped source lock, and validates SQLite/FTS integrity and paper IDs. It does not require or validate embedding artifacts. For optional embedding coverage, omit `--text-only`: the audit additionally requires complete encoding/publication and checks every expected passage ID, published ANN IDs and model provenance. This is substantial disk I/O, not a health probe. Its completion result concerns corpus integrity and coverage; fleet connectivity and retrieval quality still need separate checks.

For a separate serving database, pass `--database /path/to/library.sqlite3` to the audit while keeping the positional root pointed at the canonical NAS corpus. The native CLI selects a database through `ARXIV_DATABASE` or `--database`. These options do not publish or certify a serving copy; keep the live route unchanged until full coverage and integrity have been verified.

### Optional SSD serving copy

After ingestion finishes, publish a complete SSD generation without moving or altering the canonical NAS database:

```sh
arxiv-library-snapshot /srv/media/arxiv ~/.local/share/arxiv-serving
```

This one-shot command refuses an active importer or incomplete corpus. It requires free space for the database plus the larger of 32 GiB or 20% headroom, creates a consistent SQLite backup, performs the full text audit against NAS sources, and atomically replaces a `current` symlink only after verification. It does not enable embeddings, schedule repeated copies, or change the live MCP route.

The native path currently serves the NAS database. Snapshot-readiness integration remains separate work: verify the published receipt, full-corpus search latency and exact retrieval before pointing `ARXIV_DATABASE` at a resolved generation. The receipt is trusted local publisher output, not protection against deliberate same-user tampering.

Old generations are retained for existing readers. Remove them only after those clients exit. Failed publication can leave an unreferenced generation; interrupted processes can leave `.staging-*` directories. Inspect these before cleanup, and never remove the generation referenced by `current` or by an active client.

Hash failures stop work rather than overwriting potentially corrupt data. Inspect the failing artifact and preserve it before replacing it. Do not delete `embeddings/block-*`: the published index uses them for reranking. A changed dataset or model requires an explicit migration, not editing the pinned manifest in place.
