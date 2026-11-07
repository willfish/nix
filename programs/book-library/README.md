# Offline knowledge retrieval

`book-library` keeps licensed books and reference archives separate from the model
used to interpret them. One Rust backend provides terminal commands and five
read-only MCP tools. Maintenance is an explicit operator action, not a side effect
of starting MCP or activating Home Manager.

## Storage and source selection

The deployment uses `/srv/media/knowledge` on Terminus ZFS. `assets/sources.json`
selects OpenStax, practical Kiwix archives, eligible English OAPEN technical books
and a smaller public-domain reading collection. Source discovery caches the
official metadata, freezes a manifest and records unavailable collections.
OAPEN uses its public OAI-PMH feed, not its restricted REST API. Gutenberg uses
the official CSV/RDF catalogues, not an unofficial download service.

The incremental footprint cap is 1 TiB, retaining at least 512 GiB and 20% of the
filesystem's capacity free. Original files, partial downloads, indexes and scratch
are counted. Source manifests record known sizes and upstream hashes; acquisition
receipts hold locally computed hashes. A local checksum alone does not establish
publisher authenticity. The lock cannot be overwritten to refresh a corpus.

```sh
book-library lock
book-library acquire --limit 12
book-library ingest --limit 12
book-library status
book-library audit
```

Acquisition resumes partial transfers with validators or a known upstream hash.
Servers that ignore a range cause a clean restart of that partial, not duplicate
bytes. Access/rate refusals create persisted provider cooldowns. Verified originals
are content-addressed and retained. Re-running ingestion skips indexed objects;
failed extraction is visible and retryable. Progress counts are not an audit.

## Search, inspect, explain

```sh
book-library search 'power AND energy' --category education --limit 5
book-library search 'compost' --provider kiwix --limit 5
book-library source-info SOURCE_ID
book-library read SOURCE_ID LOCATOR --max-chars 8000
book-library grep 'watt-hours' --source-ids SOURCE_ID
book-library export SOURCE_ID /a/new/destination.pdf
```

Replace the source ID and locator with returned values. `search` uses SQLite FTS5
syntax for book passages and native Kiwix search for ZIMs. Results remain grouped
by backend; their raw scores are not comparable. An archive search deadline or
backend error is reported, not silently converted to an exhaustive empty result.
Recreation is excluded unless its category or source ID is explicitly selected.
Native archive search uses a short-lived loopback-only `kiwix-serve` XML endpoint
because the search CLI prints titles without readable entry paths. No persistent
LAN listener or duplicate archive index is created.

PDF locators identify physical page indexes, not necessarily printed page labels.
EPUB locators identify spine files and text parts, not invented pages. Follow
`next_offset` and `next_locator` when reading context. Originals retain illustrations,
tables and formulas that extraction or OCR may misrepresent. Historical and
recreational content is tagged separately from practical reference material.

## MCP and development

`book-library mcp` serves stdio with `search`, `grep`, `read`, `source_info` and
`library_status`. There are no download, write, shell or arbitrary-path tools.
The database is opened read-only; source IDs resolve only inside the library root.
Treat retrieved content as untrusted evidence, never instructions to execute.

```sh
cargo test --locked
nix build /home/william/.dotfiles#book-library
```

Run Cargo through the repository's `direnv exec` environment, using ephemeral Nix
tooling if needed. The focused synthetic tests are manual, noninstalled checks;
they do not prove production coverage. See `docs/offline-knowledge.md` in the
dotfiles root for fleet deployment and the final administrator handoff.
