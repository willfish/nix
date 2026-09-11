const HELP = '/team list | read <id> | send <id> <task> | steer <id> <guidance> | close <id|all>';
const GUIDANCE = `You coordinate a team of isolated specialists using subagent and team.
Available personas include architect (design), builder (implementation), and sceptic (independent review), alongside scout/planner/worker/reviewer.
Choose persona and task-specific skills explicitly, e.g. skills: ["rspec-testing"] for Ruby specs or ["local-dev-environment"] for Nix work. Persona defaults are loaded automatically in full.
Use independent investigations before sharing conclusions. Route concrete evidence and questions through team send to retained members; team read also retrieves results of direct user conversations. Use team steer only for guidance that does not need its own awaited answer.
Give builders explicit file ownership; do not let concurrent agents edit the same files. Subagents share filesystem access, not a sandbox. Do not invent consensus or treat repetition as independent evidence.
The main session resolves disagreements and verifies the integrated result. Four interactive panes are retained; oldest idle members may be replaced at capacity. Close unused members explicitly.`;

export function registerTeamControls(pi, getTeam, Type, StringEnum) {
  const execute = async ({ action, id, text }, signal) => {
    const team = getTeam();
    if (!team) throw new Error('Interactive teams require a root Pi session inside herdr');
    if (action === 'list') return team.list();
    if (!id) throw new Error('A team member ID is required');
    if (action === 'read') return team.read(id);
    if (action === 'close') {
      if (id === 'all') {
        for (const member of await team.list()) await team.close(member.id);
      } else await team.close(id);
      return { status: 'closed', id };
    }
    if (!text?.trim()) throw new Error('Task or guidance text is required');
    if (action === 'send') return team.send(id, text, { signal });
    if (action === 'steer') return team.steer(id, text, { signal });
    throw new Error(`Unknown team action: ${action}`);
  };
  const display = (value) => {
    // Full transcripts live in child Pi session files; avoid repeating them in parent context.
    const { messages, ...data } = Array.isArray(value) ? { members: value } : value;
    const text = JSON.stringify(data, null, 2);
    return text.length > 50000 ? `${text.slice(0, 50000)}\n[Truncated; inspect the child session for full output.]` : text;
  };
  pi.registerTool({
    name: 'team', label: 'Team',
    description: 'Coordinate retained interactive subagents. list: handles/status; read: latest result; send: follow-up in existing context and await result; steer: add guidance; close: owned pane cleanup. No peer messaging.',
    parameters: Type.Object({
      action: StringEnum(['list', 'read', 'send', 'steer', 'close']),
      id: Type.Optional(Type.String({ description: 'Member ID returned by subagent/team list; close also accepts all' })),
      text: Type.Optional(Type.String({ description: 'Task for send, or guidance for steer' })),
    }),
    async execute(_id, params, signal) {
      const value = await execute(params, signal);
      if (['error', 'aborted', 'incomplete'].includes(value.status)) throw new Error(display(value));
      return { content: [{ type: 'text', text: display(value) }] };
    },
  });
  pi.registerCommand('team', {
    description: HELP,
    async handler(args, ctx) {
      const match = args.trim().match(/^(list|read|send|steer|close)(?:\s+(\S+))?(?:\s+([\s\S]+))?$/);
      if (!match) { ctx.ui.notify(HELP, 'info'); return; }
      try {
        const value = await execute({ action: match[1], id: match[2], text: match[3] }, ctx.signal);
        pi.sendMessage({ customType: 'team', content: display(value), display: true }, { triggerTurn: false });
      } catch (error) { ctx.ui.notify(error.message, 'error'); }
    },
  });
  pi.on('before_agent_start', (event) => {
    if (getTeam()) return { systemPrompt: `${event.systemPrompt}\n\n${GUIDANCE}` };
  });
}
