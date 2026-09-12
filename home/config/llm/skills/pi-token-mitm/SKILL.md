---
name: pi-token-mitm
description: >
  Pi prompt-capture MITM token reports. Use when measuring tokens Pi transmits
  across turns, generating a histogram HTML report of skill, MCP and tool cost,
  repeating the nested harness payload capture, or comparing those reports as
  the agent setup grows.
---

# Pi token MITM reports

Before drafting or updating prose, read
`~/.agents/guides/documentation-relevance.md`.

Measure the provider request after harness transformations. Do not infer
token cost from character counts alone.

## Gates

- Nested capture spends real inference. Run it only when the user asked to
  measure, capture, or report Pi tokens.
- Catalogue match never authorizes a capture.
- Unset `PI_SESSION_FILE` and `PI_SESSION_ID` so the nested run cannot attach
  to the parent session.
- Keep `pi.jsonl` private, outside Git. Reports may keep counts and names,
  never credentials, prompt bodies, or tool contents.
- Do not serve the capture directory over HTTP. Copy only the HTML into a
  throwaway directory before opening it.

## Workflow

1. Read `references/workflow.md` for the fixture, archive paths, and reporter.
2. Reuse an existing run with `--from-run` when only the report needs a
   rebuild. Use `--capture` only for a fresh measurement.
3. Compare totals with `scripts/baseline-2026-09-12.json` and any newer files
   under `$XDG_STATE_HOME/prompt-capture/reports/`.
4. Open the HTML in the visible Brave instance using browser-automation.
   Do not launch a browser.
5. If the numbers will change harness instructions, also read
   `~/.agents/guides/harness-cost.md`. Do not edit the harness from one run.

## Verify

Run `python3 tests/test_pi_token_mitm.py` from the dotfiles checkout. Confirm
the HTML path, request count, and Brave title before claiming the report is
ready.
