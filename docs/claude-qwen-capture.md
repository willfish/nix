# Claude Code and Qwen context experiments

Home Manager installs `claude` and, on Andromeda and Relay, `qwen-claude`.
The wrappers start mitmproxy in the background for each captured run and stop
it when the command exits. No permanent proxy service is needed.

```bash
claude                           # normal Claude Code, no capture
CAPTURE_PROMPTS=1 claude          # capture normal Claude Code API traffic
qwen-claude                      # local Qwen, capture enabled by default
CAPTURE_PROMPTS=0 qwen-claude     # local Qwen without capture
```

The normal Claude wrapper follows the existing Pi, Codex and Grok convention:
an unset, empty or `0` `CAPTURE_PROMPTS` disables capture. `qwen-claude` defaults
to capture when the variable is unset; an empty value or `0` disables it.

The Qwen launcher uses a separate writable Claude profile under
`~/.config/local-llm/claude`, reads the local server credential at runtime, and
maps the main model and helper model aliases to `qwen3.8-27b`. It declares the
server's configured context window: 131,072 tokens on Andromeda and 65,536 on
Relay. It retains Claude Code's normal tool permissions and compaction behavior.

Captured Qwen requests travel through `127.0.0.1:8305` to the llama.cpp server
at `127.0.0.1:8081`, using its Anthropic Messages endpoint. The normal Claude
capture proxy listens on port 8304. Only one captured run per tool can own its
port at a time. Normal forward-proxy capture still bypasses localhost; the
Qwen launcher uses an explicit reverse-proxy endpoint to capture local traffic.

## Inspect context sizes

Logs append to `$XDG_STATE_HOME/prompt-capture/`, or
`~/.local/state/prompt-capture/` when that variable is unset:

```bash
prompt-capture logs qwen-claude
prompt-capture logs claude
```

Each HTTP request has a `run` and `flow_id`. Request records retain the full
request body, including system instructions, messages and tool definitions.
They include `model`, `request_bytes`, `request_chars`, `message_count` and
`tool_count` where applicable. Response records repeat request information;
count only `kind == "request"` when counting requests.

`kind == "usage"` records contain the backend's token usage, linked by the same
`run` and `flow_id`. Anthropic streaming sends initial input usage and later
output usage updates: merge these fields per request, rather than summing
cumulative output-token updates. Usage is captured even when response text
capture is disabled. Bytes and characters are not tokenizer counts. The input
usage describes the server's tokenized chat template, which may differ from
Claude Code's own context estimate. For Anthropic-style usage, total input
context is `input_tokens + cache_read_input_tokens + cache_creation_input_tokens`
(treat absent fields as zero). `input_tokens` alone excludes cached input.
Keep failed requests separate when
comparing sizes; they may have no usage record.

This prints one summary per request, joining its usage updates:

```bash
jq -s '
  group_by([.run, .flow_id])[] |
  (map(select(.kind == "request")) | first) as $r |
  select($r != null) |
  (reduce (.[] | select(.kind == "usage") | .usage) as $u
    ({}; . * $u)) as $usage |
  $r | {run, flow_id, model, request_bytes, request_chars,
        message_count, tool_count, usage: $usage}
' "${XDG_STATE_HOME:-$HOME/.local/state}/prompt-capture/qwen-claude.jsonl"
```

Set `PROMPT_CAPTURE_RESPONSE_BODY=1` to also record response text and streamed
chunks. Request bodies are complete; optional response text retains the
existing clipping limit. Logs are private files and redact authentication
headers, but request bodies contain the prompts and code sent to the model.

For a bounded smoke test from an empty directory:

```bash
qwen-claude -p 'Reply with exactly QWEN_OK.' \
  --tools '' --max-turns 1 --no-session-persistence
```

For a context-growth experiment, run the same task and tool configuration
across agents, retain the run IDs, and compare input usage across turns.
This setup records observations; it does not disable compaction or establish
the model's maximum reliable context length by itself.

## Local Qwen template compatibility

`home/config/local-llm/qwen3.8-chat-template.jinja` is the embedded template
extracted via `/props` from Unsloth's Qwen3.8-27B GGUF at revision
`4ca720788d1e01f1bff70c033e0d0028fd02e502`. The original template SHA-256 is
`12827f24b742ea4e80cdc12dbcf9622227056b9f797252a3149263d4f9aaadce`.
The only changed branch renders late system/developer messages as user-role
`<system-reminder>` blocks, in place. Leading system/developer merging, tool
rendering and thinking behavior are unchanged. This is a compatibility
workaround, not equivalent model-level system-role priority.

Claude Code 2.1.223's captured agent request included a `system` role after a
user message. The original template raised `System message must be at the
beginning`, returning HTTP 500 and provoking retries. Anthropic's standard
Messages schema uses the top-level `system` field instead; do not assume
mid-conversation system roles are portable across Anthropic-compatible servers.
The proxy leaves requests unchanged so wire-size measurements remain meaningful.

The shared local server loads the override via `--chat-template-file`, on
Andromeda and Relay. Only Andromeda was runtime-tested. When upgrading the
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

```bash
qwen-claude -p 'What is 2+2? Reply with just the number.' \\
  --max-turns 1 --no-session-persistence
```

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
identical workloads or compare local Qwen usage as Claude-provider billing.

## Verification and troubleshooting

```bash
systemctl --user status local-llm  # Andromeda
prompt-capture stop qwen-claude   # recover a proxy left behind after a crash
```

Claude Code's gateway and context-window configuration is documented at:

- <https://code.claude.com/docs/en/llm-gateway>
- <https://code.claude.com/docs/en/model-config>
- <https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md>
