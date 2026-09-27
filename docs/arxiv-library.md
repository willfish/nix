# arXiv deployment

Dotfiles supplies host configuration for [willfish/arxiv-mcp](https://github.com/willfish/arxiv-mcp), pinned by the `arxiv-mcp` flake input. Application code and the **[maintenance runbook](https://github.com/willfish/arxiv-mcp/blob/main/docs/maintenance.md)** live in that repository.

Use the runbook for initial setup, incremental updates, audits, backups, snapshot publication and recovery. This document covers fleet wiring and service lifecycle only.

## Hosts and transport

`home/user/arxiv-library.nix` installs the corpus runtime on Terminus and an SSH client wrapper on other hosts. `home/user/llm-mcps.nix` enables the `arxiv` catalogue entry fleet-wide.

| Role | Implementation |
| --- | --- |
| Terminus | Native C `arxiv-mcp` adapter invokes the native C `arxiv` CLI |
| Other hosts | `~/.local/bin/mcp-arxiv` connects over authenticated SSH to `william@terminus.fritz.box` |
| Maintenance | Separate Python package on Terminus; no Python MCP server |
| Canonical corpus | `/srv/media/arxiv`, on the NAS drives |
| Live database | `/srv/media/arxiv/library.sqlite3` |

No HTTP listener, extra firewall port or cross-machine database replica is needed. Each client must reach the configured hostname and authenticate non-interactively. Verify unfamiliar SSH host keys against a trusted connection rather than disabling checking.

Activate each host with `hmswitch`. Reconnect existing MCP clients to load a changed server executable or tool catalogue. A configuration entry alone does not prove that a particular host's network access and activation have been verified.

## Tools and coverage

- `search` searches complete paper bodies using BM25 literal-word AND matching and returns paper IDs and titles. An optional category filters the primary category.
- `paper` returns metadata and original assembled LaTeX. Offsets count Unicode characters; follow `next_offset` for the complete text.
- `library_status` reports the maintained FTS document count, initial import checkpoints and source settings. It is progress information, not an independent integrity audit.

Native serving is BM25-only. Optional embedding generation and index publication remain disabled by default. Existing vectors are preserved but are not exposed through a semantic/hybrid endpoint.

## Managed jobs

| Unit | Purpose | Default operation |
| --- | --- | --- |
| `arxiv-prepare` | Validate the original source manifest and pinned model assets | Dependency of corpus jobs |
| `arxiv-download` | Resume and verify source archive downloads | Initial acquisition; successful completion remains recorded |
| `arxiv-ingest` | Resume paper and BM25 import | Runs until initial import is complete; retries failures |
| `arxiv-embeddings` | Optional passage-vector generation | Opt-in |
| `arxiv-index` and its timer | Optional ANN publication | Opt-in |

```sh
systemctl --user status arxiv-ingest.service
journalctl --user -u arxiv-download -u arxiv-ingest -n 40 --no-pager
loginctl show-user william -p Linger
```

Lingering keeps user jobs alive after logout. Completed jobs can be inactive without having failed. Do not restart a healthy worker because an observation command timed out.

Package changes deliberately leave in-flight jobs running through `X-RestartIfChanged=false`. Jobs use `Type=simple` and an infinite startup timeout so adopting an existing long-running job does not kill it or block activation. The next start uses the new package. First-time adoption of unmanaged units still needs a dry-run lifecycle review; it is not equivalent to a routine package update.

The embedding worker has a 1.5-core CPU quota and 2 GB memory limit. Index publication has a one-core quota and 6 GB memory limit. These units are not part of routine BM25 maintenance.

## Updates and serving copies

There is no scheduled upstream refresh. `arxiv-library-refresh /srv/media/arxiv` checks metadata without changing the library; `--apply` is an explicit maintenance action after the initial import completes. Do not edit the original manifest in place to change revisions.

SSD publication is separate from activation. The publisher verifies a complete generation, but does not change the MCP route. Before changing the Terminus wrapper's `ARXIV_DATABASE`, follow the runbook's receipt validation, search/retrieval checks and reader-retention procedure. Select a resolved generation path, not a partial benchmark copy or an unverified database.
