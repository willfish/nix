# Pi workflow

The shared Pi setup adds a small coding workflow to the existing
[MCP integration](pi-mcp.md), [voice controls](voice.md) and
[local Qwen profile](local-llm.md). It works in `pi`, `pi-voice`, `qwen-pi`
and `qwen-pi-voice`.

| Addition | When it helps | How to use it |
| --- | --- | --- |
| Session todos | Keep multi-step work visible and preserve its checklist when resuming or branching | Ask Pi to track the work; `/todos` opens the list |
| Planning prompt | Investigate a change and agree on the approach before implementation | `/plan-work add a recording timeout` |
| Review prompt | Check a diff for concrete bugs and gaps in verification | `/review`, `/review HEAD~1`, or specify files |
| Prompt history search | Find and adapt a prompt from earlier sessions or projects | Ctrl+R, fuzzy search, Enter to restore for editing |
| Human questions | Hard-gate decisions with a selectable list, not a typed chat prompt | The `question` tool in the main pane; team `ask_coordinator` with choices |

The [official todo extension](https://github.com/earendil-works/pi/blob/v0.85.1/packages/coding-agent/examples/extensions/todo.ts)
provides one model tool, `todo`, with list, add, toggle and clear actions. Each
session keeps its own list. You can speak requests such as "track the remaining
steps" or "read my todo list" through your selected voice session. `/todos` is
an interactive list; Escape closes it.

Planning and review use Pi's built-in
[prompt templates](https://pi.dev/docs/latest/prompt-templates). They expand
only when invoked, so they do not enlarge the instructions on ordinary turns.
They are workflow instructions, not an enforced read-only mode. Planning asks
Pi to investigate, then request a go-ahead with the `question` tool and a
selectable list. Review reports findings without editing source or publishing
a review. You can also give equivalent instructions by voice.

For a typical larger change: use `/plan-work <task>`, pick from the option
list, then keep todos updated while implementing. Use `/review` when the
change is ready to inspect. Small tasks can use Pi normally.

## High-level goals

`/goal` follows Codex's persistent-goal workflow. State follows the session
branch and survives compaction. The isolated Qwen profile does not load it.

```text
/goal Document the login error contract in docs/login.md.
/goal edit
/goal status
/goal pause
/goal resume
/goal budget 100000
/goal clear
```

Setting a goal approves pursuing its objective, not bypassing credentials,
access, publishing or destructive-action gates. `/goal edit` preserves usage
and the current status; an active edit steers ongoing work to the new objective.
`/goal set <objective>` accepts literal objectives beginning with command words.
Objectives longer than 4000 characters must reference a file.

### Completion and alignment

The working model receives Codex's continuation instructions: preserve the
full requested outcome, distinguish real progress from status updates, verify
live waits, and prove every requirement against current evidence before
completing. It uses `get_goal`, `create_goal` and `update_goal`, not final-text
markers. The agent can complete, block or explicitly user-pause a goal, but
cannot change its objective, resume it or alter its budget through those tools.

Completion is the working model's evidence-backed judgment, not an independent
review or a guarantee of alignment. There is no separate auditor or `/goal
verify`. The three-consecutive-turn semantic blocker rule is model guidance;
host checks separately stop repeated empty automatic responses and explicit
execution-unavailable failures. Existing approval rules remain unchanged.

### Continuation and usage

Active goals continue when Pi has settled, without a fixed continuation count.
`/goal pause` prevents further automatic continuations; Escape also interrupts
running work. An interrupted goal stays active but does not automatically
restart in the same session until new user input or explicit resume. A paused
goal requires `/goal resume`. Restoring an active saved session resumes its goal;
forked snapshots wait for new input or explicit resume. Goals imported from the
retired auditor workflow retain their objective and require explicit resume
unless already complete.

Budgets are optional. `/goal budget none` removes a budget unless a configured
`--max-goal-token-budget` applies. Reaching a budget requests a wrap-up and sets
`budget_limited`; it is not a hard process kill. Resume preserves usage and
cannot evade an exhausted budget. Increase the budget, then resume.

Usage counts reported uncached input, cache writes and output. Team usage is
included when child snapshots are returned, not continuously between polls.
Unreported child work and compaction model calls are not included, so goal usage
is not a billing total or strict spending cap. Final provider usage-limit
messages stop separately from ordinary unrecovered errors.

The [port notes](../home/config/pi/goal/README.md) record the upstream revision
and Pi-specific lifecycle and accounting differences. No new model supervisor
or npm dependency is installed.

## Voice sessions

On Andromeda and Foundation, interactive `pi` and `qwen-pi` sessions in Herdr
automatically attach to the shared voice controller. `pi-voice` and
`qwen-pi-voice` remain compatibility commands.
The first ready non-team Pi session is selected once; later attachments and
keyboard focus do not change that destination. Choose another session from
the tray or **Super+Shift+V**, including after the selected session exits.
**Show team members** reveals otherwise hidden team children for manual selection.

The controller starts at login. Speech backends load and warm on attachment,
with loading/unavailable status in the tray; controller reconnection retries
quietly. Lost-destination dictation stays retained for explicit copy, staging
in a selected Pi editor, or discard. Recovery never auto-submits. See
[voice controls](voice.md) for the recording/send hotkeys and recovery details.

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
only writes to `auth.json` when it has to refresh the token. The isolated
Qwen profile does not load it, because its local model has no allowance to
report.

## Subagents

The standard profile ships the
[official subagent extension](https://github.com/earendil-works/pi/blob/v0.85.1/packages/coding-agent/examples/extensions/subagent/README.md),
which registers one `subagent` tool. It delegates work to a separate `pi`
session with its own context window, streams the subagent's tool calls and
progress back into the main session, and supports single, parallel and chained
runs. Four user-level agents are deployed from `home/config/pi/agents/`:

| Agent | Purpose |
| --- | --- |
| `scout` | Fast read-only recon that returns compressed context for handoff |
| `planner` | Read-only implementation planning |
| `reviewer` | Read-only code and security review of a diff |
| `worker` | General-purpose delegation with full tools |

Each role pins a default `model` and `thinking` in agent frontmatter. Those
are not advertised to the coordinator; dispatch uses the role, then the
session. The capability is meant for
cloud models (Grok, OpenAI): a subagent is a second live model conversation,
and in the local Qwen profile a concurrent request would double the KV cache
memory the GPU does not have. `qwen-pi` passes `--no-extensions` plus an
explicit list that omits the extension, so its tool set and memory footprint
are unchanged.

The three workflow prompts from the same example are deployed as
`/implement <task>` (scout, planner, worker), `/scout-and-plan <task>` and
`/implement-and-review <task>`. The tool also takes direct instructions: ask
Pi to use one agent, run several in parallel, or chain steps with the
`{previous}` placeholder.

As in the example, only user-level agents are loaded by default. Project-local
agents under `.pi/agents/` are opt-in through the tool's `agentScope`
parameter, with a confirmation prompt in untrusted projects. Team children
leave observational memory off so specialists compact normally and do not
inherit the coordinator's notes.

## History search

All four launchers use [vedang/pi-prompt-history](https://github.com/vedang/pi-prompt-history),
pinned to `eedbef7afdf16a317785be469f600d71fadc9ef0` with a verified source
hash in `home/user/pi-packages/prompt-history.nix`. Nix installs the source
without npm dependencies or install scripts. The overlay uses Pi's bundled
TUI and SQLite support. Standard Pi discovers `extensions/prompt-history/index.ts`;
Qwen explicitly loads that same package despite `--no-extensions`.

Ctrl+R opens an overlay seeded with the editor contents. Search supports fuzzy
subsequences, boosts exact matches and highlights matched characters. Local
scope means the current working directory, across sessions; Global means all
working directories within the active profile. Sections distinguish the current
session, other sessions in the same directory and other directories.

| Key | Action |
| --- | --- |
| Printable keys, Backspace, paste | Edit the search query |
| Up / Down, PageUp / PageDown | Navigate results |
| Tab or Ctrl+R inside the overlay | Toggle Local / Global scope |
| Enter | Restore the prompt into the editor and OS clipboard, without submitting |
| Esc | Cancel without changing the editor |
| F2 | Restore-session or current-session fork flow; cross-session forking is blocked |

`/prompt-history-global` opens Global scope directly. `/prompt-history-status`
reports index counts, and `/prompt-history-reindex global` rebuilds the index.
Prefer the default copy action for recall; session restoration is a separate,
more invasive action and has not been end-to-end tested. The package blocks
upstream's incompatible cross-session fork path before it can switch sessions.
Copy the prompt instead, or restore the entire session first. No model calls
are made by history search.

A small Nix-applied patch makes defaults and profile settings follow Pi's
`getAgentDir()` rather than upstream's hardcoded `~/.pi/agent`:

| Profile | Session root | SQLite index |
| --- | --- | --- |
| Standard Pi | `~/.pi/agent/sessions` | `~/.pi/agent/prompt-history/history.db` |
| Qwen | `~/.config/local-llm/pi/sessions` | `~/.config/local-llm/pi/prompt-history/history.db` |

The Qwen profile path follows Home Manager's XDG configuration directory.
Each profile reads its own `extensions/prompt-history.json` overrides. Explicit
`dbPath` and `sessionDir` overrides still work, and project settings at
`.pi/extensions/prompt-history.json` take precedence only when Pi trusts the
project. Untrusted project settings cannot redirect history storage. The active session is
also indexed even when stored outside the default session root; unrelated
project-local session directories are not automatically scanned. Indexing
refreshes when the overlay opens and skips unchanged session files.

The old custom `history-search.ts` implementation and local trial wiring are
retired. Existing prompts and the standard profile's trial index are retained.

## Configuration and updates

`home/user/pi.nix` loads `todo.ts` and the subagent extension from Pi's pinned
package, and the history overlay from its independently pinned Nix package.
It also deploys templates from `home/config/pi/` and subagent definitions from
`home/config/pi/agents/`. To update history search, change its revision/hash,
review the profile-path and safety patches and rebuild. The package build runs
the full upstream callback suite (with expectations updated for trust and the
fork guard) plus profile-isolation regressions against the bundled Pi API
in both standard and Qwen environments. No test dependencies are installed.

`home/user/local-llm.nix` explicitly loads the history extension and `todo`
for Qwen, but excludes subagents. Voice launchers inherit the same configuration.
The shared Home Manager module deploys history search on every machine; Qwen
receives it wherever the local-Qwen launcher is enabled.

After switching Home Manager, start a new Pi session to load the additions.
An existing plain Pi session can use `/reload` once to load auto-attachment;
existing Qwen sessions should
be restarted because their explicit startup arguments changed. Authentication,
model settings and session files remain writable and are not managed here.

Community planning plugins and automatic review loops were considered. The
examined plan modes require extra policy configuration for `direnv` and MCP;
repeated automatic reviews also add model work. These explicit commands cover
the initial workflow without changing ordinary coding turns.
