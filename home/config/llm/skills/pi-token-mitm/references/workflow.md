# Pi token MITM workflow

Source: local prompt-capture plus xAI Responses usage.
Checked: 2026-09-12
Update trigger: prompt-capture, Pi print-mode, or xAI usage shape changes.

## Why a nested run

The parent Pi session is not behind MITM. Capture a separate print-mode
session with `prompt-capture pi -- ...` so the jsonl records the final
provider requests, including instructions, advertised tool schemas, messages,
and returned tool content.

The default fixture is three user turns: `skill_catalog`, `mcp` status, then
write and run `add.py`. Keep that fixture when comparing harness growth.
Change it only when the user asks for a different workload.

## Commands

From the skill directory after deploy, or from
`home/config/llm/skills/pi-token-mitm` in the dotfiles checkout:

```sh
python3 scripts/report.py --capture
python3 scripts/report.py --from-run RUN_ID
```

`report.py` finds the store Pi binary from `~/.local/bin/pi` so the inner
process does not nest another prompt-capture. It writes:

- `$XDG_STATE_HOME/prompt-capture/pi.jsonl` (full capture, private)
- `$XDG_STATE_HOME/prompt-capture/reports/<run-id>.html`
- `$XDG_STATE_HOME/prompt-capture/reports/<run-id>.json`
- `$XDG_STATE_HOME/prompt-capture/reports/latest.html`

The 2026-09-12 baseline metadata is `scripts/baseline-2026-09-12.json`.
Copy any preserved HTML into `reports/` rather than into Git.

## Reading the numbers

Summing input tokens across requests counts every resend of growing context.
That is transmitted volume, not a unique-token invoice. Cached input uses the
model's cache-read price. Tool dollar figures are character-share estimates of
input cost. Schema bytes that are advertised on every turn are usually the
stable floor; skill and MCP result bodies dominate later-turn growth.

## Opening the report

Copy the HTML to a directory that contains only that file. Serve it on
loopback if `file://` is blocked. Use the existing Brave CDP on port 9222.
