const HELP = '/team list | read <id> | send <id> <task> | steer <id> <guidance> | close <id|all>';
// Tool-level policy applies to both runners; retained-pane instructions remain herdr-only.
export const DELEGATION_POLICY = 'Default to working solo unless a team is requested. Each delegation needs a distinct question or deliverable, scope, relevant skills and a stop condition. Use at most one general reviewer; add specialists only for separate evidence. Never launch every role or a fixed pipeline.';
const GUIDANCE = `Roles: scout (code map), architect (design), builder (implementation), sceptic (correctness review). Optional specialists: test-engineer (executable regressions), security-reviewer (named trust boundary), domain-specialist (authoritative business rules and acceptance examples). planner/worker/reviewer remain compatibility roles, not extra reviewers.
Use item-level skills for mixed teams; top-level skills load into every member. Defaults load in full. Do not preload unrelated workflows.
Investigate independently before sharing conclusions. Give all editing agents disjoint file ownership. When a test-engineer is assigned, reserve its named test and fixture files; builders may own other tests. Shared filesystem access is not a sandbox.
Reuse members through team send; team read includes direct user conversations, while team steer acknowledges guidance, not a completed answer. Request concise conclusions and evidence pointers, not copied source dumps.
Resolve disagreements and verify the integrated result in the main session. Stop when sufficient evidence exists. Four panes are capacity, not a staffing target; close unused members.`;

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
