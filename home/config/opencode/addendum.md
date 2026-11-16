# OpenCode addendum

You are running in OpenCode, not Pi. Follow the shared rules above. Where those rules name a Pi-only tool, use the OpenCode equivalent and do not invent the missing tool.

Skills, guides and references already live under `~/.agents`. Use `skill_catalog` to discover trusted skill IDs, then load the selected ID with OpenCode's native `skill` tool. Manual-only skills require explicit user invocation. Discovery grants no authorization. Do not copy skills into another tree.

Ask the human with OpenCode's built-in question tool, using 2 to 6 concrete options. Do not ask in the chat editor or replace this UI. Use OpenCode's native `/goal` for persistent objectives and native `todowrite` for task lists. Start a goal only on an explicit request. Hex dictates into the focused composer; submission remains explicit. OM and Jev are excluded.

Will runs OpenCode in the Herdr desktop app and prefers delegated agents as visible Herdr tabs that he can click between, not hidden OpenCode sessions. When `HC_TERMINAL_SOCKET` is set, delegate substantial or long-running work, such as a separate story, PR or independent review, to a new tab with `hc` (`~/.local/bin/herdr-core`):

1. Write a self-contained brief to a file, for example `/tmp/opencode/<name>-brief.md`. Include scope, approvals and exclusions, and any handover state.
2. Start the tab: `hc run --bg --name <name> --cwd <repo> --outlive-parent --label story=<KEY> opencode`.
3. Wait for its prompt with `hc wait <name> --timeout 60`, then submit `hc send <name> "Read <brief path> in full and carry it out."`.
4. Confirm the turn started with `hc read <name>`. Monitor with `hc wait <name>` or `hc read <name>`; do not poll with sleep.

Only send input to tabs you created. Do not close or interrupt Will's tabs. Before moving delegated work into a tab, stop any background subagent doing the same work and pass its handover in the brief. The `herdr` CLI and its `herdr --skill` instructions target a separate server that is normally not running here (`HERDR_ENV` is unset); do not bypass that check by exporting the variable, and use `hc` instead.

Use OpenCode's native subagent tool when `hc` is unavailable, or for short, bounded checks whose result returns straight into this conversation, such as parallel read-only reviews. Retain session IDs for follow-ups and use background mode for independent work. Pass completed output explicitly between dependent steps; there is no Pi chain parameter or `/team` pane scheduler. Do not spawn agents through any other shell route or delegate recursively. Shared role defaults preload their declared skills; configured permissions restrict read-only roles.

The agent bus, when this process was started with a token, exposes `list_agents`, `send_agent_message`, `list_channels`, `read_channel`, `post_channel`, `update_channel_status`, `set_coordination_scope`, `set_agent_label`, `report_work` and `get_coordination_guidance`. Those tools report and route. They do not grant permission, prove delivery, or replace the question tool. If `list_agents` is absent, do not invent bus calls.

Herdr may rename its own tab when its environment is present. Do not install or edit Herdr's OpenCode plugins from this session.

OpenCode Go uses the launcher key. Relay and Andromeda are local OpenAI-compatible providers. Connect other providers using OpenCode's own flow. Claude Agent uses the installed Claude Code login. Do not copy tokens into config or chat. MCP tools use native Code Mode discovery; search for exact tool signatures before invoking them through execute.
