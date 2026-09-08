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
