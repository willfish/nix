# OpenCode workflow

`opencode`, `code`, `co` and `c` launch OpenCode V2. Ctrl-L opens its model
picker. Shared Pi agent Markdown remains the source for specialists; deployment
translates provider names and effort variants, preloads role skills and denies
editing for declared read-only roles. Native child sessions support foreground,
background and follow-up work. Pass completed results between dependent calls.

The workflow plugin preserves native question forms and MCP Code Mode. Its
`skill_catalog` searches the current trusted registry without advertising every
skill in the initial prompt. The native skill loader retains permission checks
and explicit invocation; an unknown instruction format retains native discovery.

## Goals

Use `/goal <objective>`, `edit`, `status`, `pause`, `resume`, `budget <tokens|none>`
and `clear`. Replacing an unfinished goal asks for confirmation in the TUI.
Escape interrupts continuation. Raising a stopped budget does not resume it.
Only explicit goal requests authorize creation. The working model performs the
completion and repeated-blocker audits from the shared goal templates.

State is durable and session-scoped. Forks do not inherit a running goal.
Accounting uses reported uncached input, cache writes, visible output and
reasoning, including the session's maintenance usage and retained descendants.
Child snapshots are charged incrementally. Counts are not billing totals or a
strict spending cap; a running response can cross the budget before the next
step boundary. Closed-application time is excluded. `maxGoalTokenBudget` in the
plugin's options supplies an optional maximum and default budget.

## Providers and voice

Connect providers through `/connect`. Claude Agent uses Claude Code's own login;
complete `claude auth login` yourself, then connect Claude Agent in OpenCode.
Launcher secrets stay in the environment and outside the Nix store.

Hex runs as a Home Manager user service on Foundation and Andromeda. Super-Space
toggles dictation into the focused application, Super-Shift-Space pastes the last
dictation, and Super-Shift-V opens Hex settings. Review and submit prompts in
OpenCode. Hex owns dictation autostart; Pi's old voice tools remain available on
demand. Selected-session submission and OpenCode reply TTS are separate future
integrations. OM and Jev are not loaded by this workflow.

Restart OpenCode after switching a generation that changes the launcher or plugin.

## Manual integration check

`node home/config/opencode/check-workflow.mjs /path/to/opencode2 /path/to/built/plugin`
runs a disposable V2 server and deterministic local provider. It checks discovery,
goals, native subagents, skill preloading, restart persistence and question forms.
It uses no live model credentials and is not registered as a build or commit gate.
`OPENCODE_FIXTURE_CONFIG` and `OPENCODE_FIXTURE_AGENTS` optionally select built
configuration artifacts; `OPENCODE_FIXTURE_BUS=1` also exercises local coordination
guidance. `OPENCODE_FIXTURE_TUI=1` leaves the final question open in a real TUI for
manual interaction, then cleans up when that TUI exits.
