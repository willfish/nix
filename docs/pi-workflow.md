# Pi workflow

The shared Pi setup adds a small coding workflow to the existing
[MCP integration](pi-mcp.md), [voice controls](codex-voice.md) and
[local Qwen profile](local-llm.md). It works in `pi`, `pi-voice`, `qwen-pi`
and `qwen-pi-voice`.

| Addition | When it helps | How to use it |
| --- | --- | --- |
| Session todos | Keep multi-step work visible and preserve its checklist when resuming or branching | Ask Pi to track the work; `/todos` opens the list |
| Planning prompt | Investigate a change and agree on the approach before implementation | `/plan-work add a recording timeout` |
| Review prompt | Check a diff for concrete bugs and gaps in verification | `/review`, `/review HEAD~1`, or specify files |

The [official todo extension](https://github.com/earendil-works/pi/blob/v0.85.1/packages/coding-agent/examples/extensions/todo.ts)
provides one model tool, `todo`, with list, add, toggle and clear actions. Each
session keeps its own list. You can speak requests such as "track the remaining
steps" or "read my todo list" through your normal voice launcher. `/todos` is
an interactive list; Escape closes it.

Planning and review use Pi's built-in
[prompt templates](https://pi.dev/docs/latest/prompt-templates). They expand
only when invoked, so they do not enlarge the instructions on ordinary turns.
They are workflow instructions, not an enforced read-only mode. Planning asks
Pi to investigate and wait for an instruction to implement. Review reports
findings without editing source or publishing a review. You can also give
equivalent instructions by voice.

For a typical larger change: use `/plan-work <task>`, discuss the proposal,
then ask Pi to implement and keep its todos updated. Use `/review` when the
change is ready to inspect. Small tasks can use Pi normally.

Session naming (`/name`), navigation (`/tree`), resume (`/resume`) and compaction
are already provided by Pi. The existing MCP adapter supplies your external
integrations. The configuration keeps those facilities and adds no background
services, model calls at startup or new npm dependencies.

## Session context budget

In `pi` and `pi-voice`, `/context` opens a picker for the
`openai-codex/gpt-6-astra` subscription model:

| Preset | Context ceiling | Command |
| --- | --- | --- |
| Lean (default) | 272k | `/context lean` |
| Extended | 500k | `/context extended` |
| Maximum | 872k | `/context maximum` |

Numeric aliases `/context 272k`, `/context 500k` and `/context 872k` also work.
The footer shows the active ceiling. Changes require an idle session and affect
only that session's model, not `models.json`, authentication, output limits or
startup defaults. The choice follows the active session branch through resume,
reload and model switching. Forks inherit choices on their copied branch;
new sessions start lean. Other models and the isolated Qwen profile are unchanged.

The 872k maximum comes from Astra's locally cached Codex catalogue on
2026-09-09, not a successful large-request test. Extended and Maximum are
labelled backend-untested. Backend access and subscription usage rules still
apply; this command cannot increase an account's entitlement.

A larger ceiling permits more history before auto-compaction; it does not
immediately fill the context. Pi still subtracts its configured response reserve
before compacting. Reducing below current usage (or with unknown usage) asks for
confirmation and may cause lossy auto-compaction on the next turn. The command
itself does not compact. Increasing the ceiling cannot recover already compacted
details. Choose the larger budget before loading a large legal document set,
and retain original sources for quotation and citation checks.

## Subscription allowance

`/usage` reports how much allowance is left for the model active in the
session. For `openai-codex` it queries the ChatGPT backend `wham/usage`
endpoint with the stored OAuth token (refreshing it through the same grant
Pi's login flow uses when the access token is expired) and shows the
remaining percentage of the rate-limit window, its reset time, whether the
active model is currently available, and any reset credits. Other models are
usage-based and have no subscription window, so the command reports the
session context figure instead. It makes one HTTP request per invocation and
only writes to `auth.json` when it has to refresh the token.

## Configuration and updates

`home/user/pi.nix` loads `todo.ts` from the same pinned Nix package as Pi and
deploys the two templates from `home/config/pi/prompts/`. There is no separate
plugin version to update. `home/user/local-llm.nix` explicitly loads those
resources for the isolated Qwen profile and includes `todo` in its tool list.
The voice launchers inherit the same configuration.

After switching Home Manager, start a new Pi session to load the additions.
An existing plain Pi session can use `/reload`; existing Qwen sessions should
be restarted because their explicit startup arguments changed. Authentication,
model settings and session files remain writable and are not managed here.

Community planning plugins and automatic review loops were considered. The
examined plan modes require extra policy configuration for `direnv` and MCP;
repeated automatic reviews also add model work. These explicit commands cover
the initial workflow without changing ordinary coding turns.
