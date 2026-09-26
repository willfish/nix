# Local Qwen template compatibility

`home/config/local-llm/qwen3.8-chat-template.jinja` is the embedded template
extracted via `/props` from Unsloth's Qwen3.8-27B GGUF at revision
`4ca720788d1e01f1bff70c033e0d0028fd02e502`. The original template SHA-256 is
`12827f24b742ea4e80cdc12dbcf9622227056b9f797252a3149263d4f9aaadce`.
The only changed branch renders late system/developer messages as user-role
`<system-reminder>` blocks, in place. Leading system/developer merging, tool
rendering and thinking behavior are unchanged. This is a compatibility
workaround, not equivalent model-level system-role priority.

A captured agent request included a `system` role after a user message. The
original template raised `System message must be at the beginning`, returning
HTTP 500 and provoking retries. Some chat APIs put system text in a top-level
field instead; do not assume mid-conversation system roles are portable.
The proxy leaves requests unchanged so wire-size measurements remain meaningful.

Andromeda loads the override via `--chat-template-file` and was runtime-tested.
Relay's Huihui Qwen3.6 model uses its own embedded template instead. When upgrading the
model, compare against its embedded template before retaining this override.
To revert, remove that argument from `home/user/local-llm.nix`, run `hmswitch`,
and restart the local server. The GGUF itself is not modified.

Regression tests:

```bash
nix-shell -p 'python3.withPackages (ps: [ ps.jinja2 ])' \\
  --run 'python3 tests/test_qwen_chat_template.py'
```

### Verified capture, 2026-09-09 (Andromeda)

From an empty temporary directory, with normal tools and a 180-second outer
timeout:

The capture was a one-turn local agent request from that empty directory.

Run `20260909T103848-3054610` returned `4`, exit 0:

| Measurement | Observed |
|---|---:|
| Model requests | 1, HTTP 200, no retries |
| Request characters / UTF-8 bytes | 78,355 / 78,756 |
| Tools | 24 |
| Uncached input tokens | 3,139 |
| Cache-read input tokens | 16,384 |
| Total reported input context | 19,523 tokens |
| Output tokens | 2 |
| End-to-end wall time | 37.371 seconds |
| Server prompt processing + generation | 1.490 seconds |

Wall time includes waiting behind another client on the single server slot;
it is **not** an uncontended latency benchmark. This was a warm-cache run.
An earlier bounded attempt timed out at 120 seconds under contention and is
excluded. A separate empty-body probe returned 401, not a model request.
There was no preflight generation in this successful run, unlike the earlier
experiment, and the tool count changed from 22 to 24. Do not treat the runs as
identical workloads or compare local Qwen usage as provider billing.

For this usage report, total input context is the sum of `input_tokens`,
`cache_read_input_tokens` and `cache_creation_input_tokens` (missing fields
count as zero). Character counts are not tokenizer counts or billing costs.
