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

The [official todo extension](https://github.com/earendil-works/pi/blob/v0.85.1/packages/coding-agent/examples/extensions/todo.ts)
provides one model tool, `todo`, with list, add, toggle and clear actions. Each
session keeps its own list. You can speak requests such as "track the remaining
steps" or "read my todo list" through your selected voice session. `/todos` is
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

## High-level goals

`/goal` preserves a user-owned objective and requires independent review before
accepting completion. State follows the active session branch and survives
compaction. The isolated Qwen profile does not load this extension.

```text
/goal Document the login error contract in docs/login.md.
/goal edit
/goal status
/goal pause
/goal resume
/goal verify
/goal clear
```

Use `/goal edit` to put explicit acceptance criteria on separate lines. Each
nonempty line is an audit item; every clause and referenced requirement still
needs evidence. Only user commands change the objective. Editing increments its
revision, clears old audit evidence and pauses it. `/goal set <objective>` also
allows literal objectives beginning with command words such as `pause`.

### Completion and evidence

A final completion marker requests an audit; it does not mark the goal complete.
A fresh Pi process receives the contract, not the implementing conversation.
It loads no extensions, skills, context files or prompt templates and exposes
only `read`, `grep`, `find` and `ls`, never `bash`, `write` or `edit`.

The auditor must return structured evidence for every contract item. The
extension checks coverage, verdict consistency, audit identity, goal revision,
successful read-only inspection and successful process exit. Invalid, partial,
failed or stale responses cannot complete a goal. Reports remain available in
`/goal status` and session entries.

Each audit has a compact card in the transcript showing elapsed time, the auditor
model and its latest file/tool inspection. Click the card to expand it in Pi's
fullscreen mode. In either terminal mode, use Pi's tool-output expansion binding
(default Ctrl+O) to expand or collapse details globally. The last twelve tool
activities are retained without raw file contents, search patterns, or model
reasoning. After exit, the card keeps its verdict, requirement counts and
expandable evidence. Cancelled or interrupted audits are labelled explicitly,
never presented as successful reviews. Use `/goal pause` to cancel a running
audit; the editor stays available while it works.

Cards are TUI-only session entries, not messages sent to the model. Heartbeats
update the live display without appending transcript entries every second.

- `PASS`: every item has direct supporting evidence; the goal becomes complete.
- `FAIL`: at least one requirement is demonstrably unmet. An active goal can
  continue with the objections, within its remaining allowance.
- `UNVERIFIED`: evidence is missing or requires unavailable capabilities. Work
  stops for inspection, rather than looping until the auditor agrees. Transport
  errors, timeouts and malformed reports also leave completion unverified.

**Read-only review cannot prove everything.** The auditor cannot rerun tests,
check live services or prove a push reached GitHub. Saved logs and the worker's
assertions are not independent runtime verification. Such requirements remain
unverified; inspect them separately rather than weakening the contract to get a
PASS. This is model judgment with reduced self-justification, not a truth oracle
or an operating-system sandbox. Read tools still have the process's filesystem
access, and concurrent external edits are not locked out by an audit.

### Control and limits

`/goal pause` cancels the auditor and prevents new goal continuations. It does
not abort implementation work already running; use Escape to stop that too.
Escape stops goal automation without another confirmation. Pending human
questions and the agent's explicit waiting marker pause immediately. An ordinary
answer does not automatically resume a paused goal: use `/goal resume`.

Each start or explicit resume allows ten automatic continuations and three audit
attempts. The initial start/resume turn is separate from the ten continuations.
An audit has a three-minute timeout and a bounded output buffer. These are
scheduling limits, not token or billing caps on a long model/tool turn. Editing
and `/goal verify` do not reset allowances. Inspect progress before resuming.

`/goal verify` requires an idle session and does not restart implementation when
a paused goal fails review. New input or agent work cancels an in-flight audit.
Edits, replacements, pause, clear, tree navigation and session shutdown invalidate
old audit results. Restored unfinished goals require explicit resume; legacy
completion claims require a new audit. A completed goal stays closed on resume.

This keeps the isolated-auditor idea without installing another task supervisor
alongside the existing team system. Self-grading goal trackers offer useful
accounting but do not provide this completion gate. More comprehensive systems
such as [GLLA](https://github.com/DraconDev/pi-goal-list-loop-audit) also own task
queues, recovery and worker supervision, which remain separate here.

Session naming (`/name`), navigation (`/tree`), resume (`/resume`) and compaction
are already provided by Pi. The existing MCP adapter supplies your external
integrations. These workflow additions keep those facilities and add no model
calls at startup or new npm dependencies.

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

None of the definitions pin a model, so each subagent runs on the model and
thinking level the dispatching session is using. The capability is meant for
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
parameter, with a confirmation prompt in untrusted projects.

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
