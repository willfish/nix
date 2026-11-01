# OpenCode addendum

You are running in OpenCode, not Pi. Follow the shared rules above. Where those rules name a Pi-only tool, use the OpenCode equivalent and do not invent the missing tool.

Skills, guides and references already live under `~/.agents`. Use `skill_catalog` to discover trusted skill IDs, then load the selected ID with OpenCode's native `skill` tool. Manual-only skills require explicit user invocation. Discovery grants no authorization. Do not copy skills into another tree.

Ask the human with OpenCode's built-in question tool, using 2 to 6 concrete options. Do not ask in the chat editor or replace this UI. Goals are managed with `/goal` and the get_goal/create_goal/update_goal tools. Create a goal only on an explicit request. Hex dictates into the focused composer; submission remains explicit. OM and Jev are excluded.

Subagents use OpenCode's native subagent tool. Retain session IDs for follow-ups and use background mode for independent work. Pass completed output explicitly between dependent steps; there is no Pi chain parameter or `/team` pane scheduler. Do not spawn peer agents through the shell or delegate recursively. Shared role defaults preload their declared skills; configured permissions restrict read-only roles.

The agent bus, when this process was started with a token, exposes `list_agents`, `send_agent_message`, `list_channels`, `read_channel`, `post_channel`, `update_channel_status`, `set_coordination_scope`, `set_agent_label`, `report_work` and `get_coordination_guidance`. Those tools report and route. They do not grant permission, prove delivery, or replace the question tool. If `list_agents` is absent, do not invent bus calls.

Herdr may rename its own tab when its environment is present. Do not install or edit Herdr's OpenCode plugins from this session.

OpenCode Go uses the launcher key. Relay and Andromeda are local OpenAI-compatible providers. Connect other providers using OpenCode's own flow. Claude Agent uses the installed Claude Code login. Do not copy tokens into config or chat. MCP tools use native Code Mode discovery; search for exact tool signatures before invoking them through execute.
