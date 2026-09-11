# Agent models in Herdr

The companion integrations report display-only metadata through
`pane.report_metadata`. No Herdr patch, screen scraping, lifecycle overrides,
renaming of panes, or model API calls are involved.

The visible label is, for example, `pi · medium · gpt-6-astra`.
Effort comes first so long model names cannot push
it off a narrow sidebar. Herdr can still clip long labels; widen the sidebar
if necessary. Values are limited to Herdr's 80-character display label cap.

## Supported sources

- **Pi:** `ctx.model.id` and `ctx.thinkingLevel`, refreshed at session start,
  model/thinking selection, agent start and tree navigation. Only TUI sessions
  report, so inherited pane variables in RPC/headless subagents do not overwrite
  the parent. A five-second heartbeat renews a 15-second TTL. Shutdown clears
  the extension's display override, including reload and session replacement.

The standard Pi profile auto-discovers the extension. `qwen-pi` uses a separate
profile with explicit extension flags and is not covered by this wiring.

## Apply and verify

Build the appropriate Home Manager activation package, then use `hmswitch`.
Existing Pi sessions need `/reload`. This does not require restarting the Herdr server.

From a Herdr pane, `herdr pane get "$HERDR_PANE_ID"` exposes `display_agent`.
The semantic `agent` and `agent_status` fields must remain unchanged. Select a
different model/effort and check again. Socket errors/timeouts are silent and
bounded to half a second; metadata failure must never block agent work.

Tests:

```sh
direnv exec . node --test tests/pi-herdr-model.test.mjs
direnv exec . env PI_HERDR_TEST_BIN=pi python3 -m unittest discover -s tests -p test_herdr_pi_runtime.py
```

The runtime test starts an isolated offline Pi TUI with fake credentials and a
local mock socket. It switches effort and model without sending an LLM request,
then checks that shutdown clears the label.
