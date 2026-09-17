import { formatJob } from './job-results.ts';
import { promptWithChoices } from '../question.ts';

const HELP = '/team list | questions | read <member|job> | send <member> <task> | steer <member> <guidance> | answer <question> <text> | ask <question> | wait <job> | cancel <job> | close <member|all>';
export const DELEGATION_POLICY = 'Default to working solo unless a team is requested. Each delegation needs a distinct question or deliverable, scope, relevant skills and a stop condition. Use at most one general reviewer; add specialists only for separate evidence. Never launch every role or a fixed pipeline.';
const GUIDANCE = `Roles: scout (code map), architect (design), builder (implementation), sceptic (correctness review). Optional: test-engineer (regressions), security-reviewer (trust boundaries), domain-specialist (authoritative rules). planner/worker/reviewer are compatibility roles.
Use item-level skills for mixed teams; top-level skills load into every member. Give editing agents disjoint ownership, including named tests. Shared filesystem access is not a sandbox.
Call subagent or team send/wait/ask alone, using subagent tasks for parallelism. A waiting_question result is a paused job, not a completed task: never restart its chain or substitute the question as previous output.
Answer pending questions with team answer from existing evidence or an architect recommendation. Use team ask only for credentials, access, unapproved live/destructive work, or mandatory gates; this asks the human in the main pane as a selectable list. Always include concrete choices. Design, approach, and plan choices are not human questions. Do not invent human approval. If a dialog is dismissed, leave the question pending and do not reopen it automatically.
Answers acknowledge queued input, not completion. Use team wait for the job's next question or final result; team questions lists pending questions. Explicit team cancel stops a job after it yielded. The main pane receives later question/completion notifications; no pane hopping is required.
Reuse team send for retained context; team read includes direct user conversations. team steer is guidance, not an answer to a pending question. Request concise evidence, resolve disagreements and verify integration. Four panes are capacity, not a staffing target; waiting questions retain their slots.`;
const terminal = new Set(['completed', 'error', 'aborted']);
export function publicJob(snapshot) {
  return { ...snapshot, tasks: snapshot.tasks.map(task => ({ ...task,
    ...(task.result ? { result: Object.fromEntries(Object.entries(task.result).filter(([key]) => key !== 'messages')) } : {}),
  })) };
}

export function registerTeamControls(pi, getTeam, Type, StringEnum, getJobs = () => undefined) {
  const seen = new Map(), timers = new Map(), offeredHuman = new Set();
  let uiCtx;
  const noteUi = ctx => { if (ctx?.mode === 'tui' && ctx.ui) uiCtx = ctx; };
  const remember = snapshot => seen.set(snapshot.jobId, {
    revision: snapshot.revision, terminal: terminal.has(snapshot.status),
    questions: new Set(snapshot.tasks.flatMap(task => task.state === 'waiting_question' ? [task.result.question.id] : [])),
    cleanup: new Set(snapshot.tasks.filter(task => task.cleanupError).map(task => `${task.index}:${task.cleanupError}`)),
  });
  const changed = snapshot => {
    if (!seen.has(snapshot.jobId) || timers.has(snapshot.jobId)) return;
    const timer = setTimeout(() => {
      timers.delete(snapshot.jobId);
      const jobs = getJobs(), prior = seen.get(snapshot.jobId);
      if (!jobs || !prior) return;
      const current = jobs.snapshot(snapshot.jobId);
      const fresh = current.tasks.some(task => task.state === 'waiting_question' && !prior.questions.has(task.result.question.id));
      const cleanupFailed = current.tasks.some(task => task.cleanupError && !prior.cleanup.has(`${task.index}:${task.cleanupError}`));
      if (current.revision <= prior.revision || (!fresh && !cleanupFailed && (!terminal.has(current.status) || prior.terminal))) return;
      remember(current);
      const human = current.tasks.find(task => task.state === 'waiting_question' && task.result.question.requiresUser
        && task.result.question.choices?.length && !prior.questions.has(task.result.question.id));
      if (human && uiCtx?.mode === 'tui') {
        void presentHuman(human.result.question, `${human.agent}: ${human.result.question.text}`, uiCtx, undefined, jobs);
        return;
      }
      pi.sendMessage({ customType: 'team-job', content: formatJob(current), display: true }, { triggerTurn: true, deliverAs: 'followUp' });
    }, 50);
    timer.unref?.();
    timers.set(snapshot.jobId, timer);
  };
  const visible = snapshot => {
    remember(snapshot);
    const current = getJobs()?.snapshot(snapshot.jobId);
    if (current && current.revision > snapshot.revision) changed(current);
  };
  const wait = async (id, signal, after) => {
    const snapshot = await getJobs().wait(id, { signal, after, timeoutMs: 30000 });
    visible(snapshot);
    return snapshot;
  };
  async function presentHuman(question, title, ctx, signal, jobs) {
    if (!question?.requiresUser || !question.choices?.length || offeredHuman.has(question.id)) return;
    if (ctx?.mode !== 'tui' || !ctx.ui) return;
    offeredHuman.add(question.id);
    const text = await promptWithChoices(ctx.ui, title, question.choices, signal);
    if (signal?.aborted) throw new Error('Human question cancelled; the question is still pending');
    if (text === undefined || text === null || !String(text).trim()) {
      return { status: 'waiting_question', questionId: question.id, text: 'Dismissed; still waiting. Do not reopen automatically.' };
    }
    return jobs.answer(question.id, String(text).trim(), { source: 'human' });
  }
  async function presentFromSnapshot(snapshot, signal, ctx, jobs) {
    const task = snapshot.tasks.find(item => item.state === 'waiting_question' && item.result?.question?.requiresUser && item.result.question.choices?.length);
    if (!task) return;
    return presentHuman(task.result.question, `${task.agent}: ${task.result.question.text}`, ctx, signal, jobs);
  }
  const execute = async ({ action, id, text, after }, signal, ctx, source) => {
    noteUi(ctx);
    const team = getTeam(), jobs = getJobs();
    if (signal?.aborted) throw new Error('Team action was cancelled');
    if (!team) throw new Error('Interactive teams require a root Pi session inside herdr');
    if (action === 'list') return team.list();
    if (action === 'questions') return jobs?.questions() ?? [];
    if (!id) throw new Error('A member, job, or question ID is required');
    if (action === 'read') return jobs?.jobs.has(id) ? publicJob(jobs.snapshot(id)) : team.read(id);
    if (action === 'wait') {
      const snapshot = await wait(id, signal, after);
      return await presentFromSnapshot(snapshot, signal, ctx, jobs) ?? publicJob(snapshot);
    }
    if (action === 'cancel') {
      const snapshot = jobs.cancel(id);
      visible(snapshot);
      return publicJob(snapshot);
    }
    if (action === 'answer' || action === 'ask') {
      if (!jobs) throw new Error('Question registry is unavailable');
      if (action === 'ask') {
        const q = jobs.question(id);
        if (!q) throw new Error('Unknown or stale question');
        if (ctx?.mode !== 'tui') throw new Error('Human answers require the main interactive Pi pane');
        offeredHuman.add(q.id);
        const title = `${jobs.snapshot(q.jobId).tasks[q.index].agent}: ${q.text}`;
        text = await promptWithChoices(ctx.ui, title, q.choices, signal);
        if (signal?.aborted) throw new Error('Human question cancelled; the question is still pending');
        if (text === undefined || text === null || !text.trim()) return {
          status: 'waiting_question', questionId: id, text: 'Dismissed; still waiting. Do not reopen automatically.',
        };
        source = 'human'; // Only actual UI input or the user slash-handler can supply this provenance.
      }
      return jobs.answer(id, text, { source });
    }
    if (action === 'close') {
      if (id === 'all') {
        const members = await team.list();
        if (jobs) for (const jobId of jobs.jobs.keys()) jobs.cancel(jobId);
        const errors = [];
        for (const member of members) {
          try {
            await team.close(member.id);
            jobs?.memberClosed(member.id);
          } catch (error) { errors.push(error); }
        }
        if (errors.length) throw new AggregateError(errors, `Could not close ${errors.length} team member(s): ${errors.map(error => error.message).join('; ')}`);
      } else {
        await team.close(id);
        jobs?.memberClosed(id);
      }
      return { status: 'closed', id };
    }
    if (!text?.trim()) throw new Error('Task or guidance text is required');
    if (action === 'send') {
      if (!jobs) return team.send(id, text, { signal });
      const member = team.get(id);
      const state = await team.state(member);
      if (state?.question) throw new Error('Coordinator question pending; answer or cancel it first');
      if (member.pending || !state?.idle) throw new Error('Agent is busy; use steer');
      const jobId = jobs.start({ mode: 'single', tasks: [{ agent: member.agent }],
        run: (_task, _index, _previous, ownedSignal) => team.send(id, text, { signal: ownedSignal }),
        resume: (outcome, answer, origin, ownedSignal) => team.answer(outcome.memberId, outcome.question.id, answer, { source: origin, signal: ownedSignal }),
        cancelMember: memberId => team.close(memberId), health: memberId => team.health(memberId),
      });
      const snapshot = await wait(jobId, signal);
      const presented = await presentFromSnapshot(snapshot, signal, ctx, jobs);
      if (presented) return presented;
      const result = snapshot.tasks[0].result;
      return terminal.has(snapshot.status) && result ? { ...result, jobId, revision: snapshot.revision } : publicJob(snapshot);
    }
    if (action === 'steer') return team.steer(id, text, { signal });
    throw new Error(`Unknown team action: ${action}`);
  };
  const display = value => {
    const { messages, ...data } = Array.isArray(value) ? { members: value } : value;
    const text = JSON.stringify(data, null, 2);
    return text.length > 50000 ? `${text.slice(0, 50000)}\n[Truncated; inspect the child session for full output.]` : text;
  };
  pi.registerTool({
    name: 'team', label: 'Team',
    description: 'Coordinate retained agents and resumable jobs. questions lists pending questions; answer queues a coordinator answer; ask gets a human answer in this pane; wait awaits job progress; cancel stops a job. send starts a follow-up job. read accepts member/job ID. close cleans owned panes. No peer messaging.',
    parameters: Type.Object({
      action: StringEnum(['list', 'read', 'send', 'steer', 'close', 'questions', 'answer', 'ask', 'wait', 'cancel']),
      id: Type.Optional(Type.String({ description: 'Member ID for send/steer/close; job ID for wait/cancel; question ID for answer/ask' })),
      text: Type.Optional(Type.String({ description: 'Task, guidance or answer text; no human-provenance override' })),
      after: Type.Optional(Type.Number({ description: 'Optional last-seen job revision for wait' })),
    }),
    async execute(_id, params, signal, _update, ctx) {
      const value = await execute(params, signal, ctx, 'coordinator');
      if (['error', 'aborted', 'incomplete'].includes(value.status) && params.action !== 'cancel') throw new Error(display(value));
      return { content: [{ type: 'text', text: display(value) }] };
    },
  });
  pi.registerCommand('team', {
    description: HELP,
    async handler(args, ctx) {
      const match = args.trim().match(/^(list|questions|read|send|steer|close|answer|ask|wait|cancel)(?:\s+(\S+))?(?:\s+([\s\S]+))?$/);
      if (!match) { ctx.ui.notify(HELP, 'info'); return; }
      try {
        const value = await execute({ action: match[1], id: match[2], text: match[3] }, ctx.signal, ctx, 'human');
        pi.sendMessage({ customType: 'team', content: display(value), display: true }, { triggerTurn: ['answer', 'ask'].includes(match[1]) && value.status === 'queued', deliverAs: 'followUp' });
      } catch (error) { ctx.ui.notify(error.message, 'error'); }
    },
  });
  pi.on('before_agent_start', (event, ctx) => {
    noteUi(ctx);
    if (getTeam()) return { systemPrompt: `${event.systemPrompt}\n\n${GUIDANCE}` };
  });
  return { changed, visible, reset() {
    for (const timer of timers.values()) clearTimeout(timer); timers.clear(); seen.clear(); offeredHuman.clear(); uiCtx = undefined;
  } };
}
