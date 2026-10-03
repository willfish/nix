# OpenCode addendum

You are running in OpenCode, not Pi. Follow the shared rules above. Where those rules name a Pi-only tool, use the OpenCode equivalent and do not invent the missing tool.

Skills, guides and references already live under `~/.agents`. OpenCode loads those skills itself. Do not copy them into an OpenCode skill tree, and do not look for a Pi skill catalogue tool.

Ask the human with OpenCode's built-in question tool, using 2 to 6 concrete options. Do not ask in the chat editor. Todos are the built-in todo tool. There is no `/goal` tool and no voice dictation bridge in this setup. Observational memory is off. Do not start either.

Subagents are OpenCode agents invoked with the task tool. Do not spawn peer agents through the shell. Pi tool allowlists are not enforced here.

The agent bus, when this process was started with a token, exposes `list_agents`, `send_agent_message`, `list_channels`, `read_channel`, `post_channel`, `update_channel_status`, `set_coordination_scope`, `set_agent_label`, `report_work` and `get_coordination_guidance`. Those tools report and route. They do not grant permission, prove delivery, or replace the question tool. If `list_agents` is absent, do not invent bus calls.

Herdr may rename its own tab when its environment is present. Do not install or edit Herdr's OpenCode plugins from this session.

OpenCode Go uses the launcher key. Relay and Andromeda are local OpenAI-compatible providers. xAI still needs OpenCode's own connect flow if it is not already authenticated. Do not copy tokens into config or chat.
