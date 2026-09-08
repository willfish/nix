# Knowledge base embeddings

Andromeda runs Qwen3-Embedding-0.6B Q8_0 through a localhost-only
`llama-server` on port 8082, using eight CPU threads. This reserves Andromeda's
RTX 5090 memory for the local coding model, speech and desktop. Home Manager pins
both the model revision and SHA-256. Other hosts continue to use `kb-search`
SSH fallback when they have no local database.

The CPU migration retains the same model, pooling and preprocessing. A public
query returned 1,024 finite dimensions in 0.218 seconds, with cosine similarity
0.99967 to the previous Vulkan result. A representative 6,000-byte document
chunk took 8.459 seconds on 2026-09-08, within the client's 180-second timeout.

## Contract

- Model ID: `qwen3-embedding-0.6b-q8_0-chunks-v1`
- 1,024 dimensions, L2 normalized, last-token pooling in llama.cpp.
- Documents: title plus **entire** body, no Nomic prefix or 24K-character cutoff.
- Long documents: overlapping chunks of at most 6,000 UTF-8 bytes with a
  400-byte overlap. Normalize each chunk, sum and normalize the document vector.
  Retrieval is document-level, not independent passage retrieval. Pooling can
  dilute isolated facts in very long documents.
- Queries: Qwen's `Instruct: ...\nQuery: ...` retrieval instruction, embedded
  at search time. Oversized queries fail explicitly rather than being truncated.
- Changing weights, preprocessing or dimensions requires a new contract ID and
  a full re-embed. Do not mix vectors from different models.

## Search and freshness

`kb-search --json "query"` uses the backend matching the stored index. On
Andromeda, the packaged `auto` backend defaults to Qwen. For Qwen searches,
curated Markdown is reconciled using the existing database/file ownership rules,
then missing or stale vectors are generated before the query vector. Unchanged
vectors are reused. Embedding refreshes are serialized with a database-adjacent
file lock, so a search can wait for a running corpus embedding job.

The same process covers Confluence, Jira, curated session outcomes, notes,
compressed summaries and light Slack `CHANNEL.md` documents. Raw Slack dumps
and unreviewed `.raw/` session drafts remain intentionally excluded. Database-owned
content remains authoritative over conflicting Markdown imports.

If inference fails, Qwen search fails visibly. It never substitutes hash vectors.
Use `kb-search --mode bm25 "query"` for explicit keyword-only operation while
inference is unavailable. The first search after substantial changes can be slow;
the daily job normally handles bulk indexing and embeddings in advance.

A copied Qwen database on another host needs the same model endpoint. Set
`KNOWLEDGE_BASE_EMBED_URL` to a reachable compatible service, or use
`kb-search --remote william@andromeda "query"`. The service does not listen on
external interfaces; use SSH forwarding rather than exposing it publicly.

## Operations

```sh
systemctl --user status knowledge-base-embeddings
curl -fsS http://127.0.0.1:8082/health
journalctl --user -u knowledge-base-embeddings -n 50
knowledge-base index
knowledge-base embed --backend qwen
kb-search --json "quota balance"
```

Before migrating an existing index, use SQLite's backup API to save
`.kb/knowledge.sqlite` and copy `.kb/embed-config.json`. Do not copy a live WAL
SQLite database with plain `cp`. Re-embedding is incremental and retryable;
check `errors == 0` and `total_in_index == documents` in its summary. For a
freshness check, a second embedding run should embed zero documents and skip
all documents. Keep the backup until search and per-source coverage are verified.

Configuration is in `home/user/knowledge-base.nix`; backend implementation is
`home/config/llm/scripts/knowledge_base/qwen_embed.py`.
