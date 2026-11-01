# OpenCode workflow

`opencode`, `code`, `co` and `c` launch OpenCode V2. Ctrl-L opens its model
picker. Shared Pi agent Markdown remains the source for specialists; deployment
translates provider names and effort variants, preloads role skills and denies
editing for declared read-only roles. Native child sessions support foreground,
background and follow-up work. Pass completed results between dependent calls.

Tool operations are auto-allowed globally, matching Pi's approval-free workflow,
including environment-file reads and paths outside the project. Specialist role
limits still apply. Native questions remain available for human decisions.

The workflow plugin preserves native question forms and MCP Code Mode. Its
`skill_catalog` searches the current trusted registry without advertising every
skill in the initial prompt. The native skill loader retains permission checks
and explicit invocation; an unknown instruction format retains native discovery.

## Native goals, todos and subagents

Use OpenCode V2's built-in `/goal <objective>`, `/goal pause`, `/goal resume`
and `/goal clear`. Its native goal indicator shows status and opens the goal
dialog. OpenCode owns persistence, continuation, progress review and stop guards.

Native `todowrite` and the todo UI are enabled through `experimental.todo`.
The workflow plugin adds no goal, todo or subagent implementation. Shared
specialist profiles run through the native `subagent` tool with retained session
IDs, model variants, foreground/background work and ordinary question forms.

Native `/review` handles uncommitted changes, commits, branches and pull requests.
The primary-only addendum supplies design routing; shared rules come from
`AGENTS.md`. Pi's spoken-summary formatting is confined to the Pi harness.

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
native goals and todos, subagents, skill preloading and question forms.
It uses no live model credentials and is not registered as a build or commit gate.
`OPENCODE_FIXTURE_CONFIG`, `OPENCODE_FIXTURE_AGENTS`, `OPENCODE_FIXTURE_COMMANDS`
and `OPENCODE_FIXTURE_ORCHESTRATOR` optionally select built configuration artifacts.
`OPENCODE_FIXTURE_BUS=1` also exercises local coordination
guidance. `OPENCODE_FIXTURE_TUI=1` leaves the final question open in a real TUI for
manual interaction, then cleans up when that TUI exits.
