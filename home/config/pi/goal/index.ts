// Codex goal behaviour adapted to Pi's session journal and settled-agent lifecycle.
// Upstream provenance and the Apache-2.0 license are alongside this module.
import { randomUUID } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';

export const STATE_TYPE = 'goal';
const CONTEXT_TYPE = 'goal-context';
const STATUSES = ['active', 'paused', 'blocked', 'usage_limited', 'budget_limited', 'complete'];
const templates = Object.fromEntries(['continuation', 'budget_limit', 'objective_updated'].map(name =>
  [name, readFileSync(new URL(`./templates/${name}.md`, import.meta.url), 'utf8')]));
const HELP = '/goal <objective> | set <objective> | edit [objective] | status | pause | resume | budget <tokens|none> | clear';
const integer = value => Number.isSafeInteger(value) && value >= 0 ? value : 0;

export function validateObjective(value) {
  const objective = String(value ?? '').trim();
  if (!objective) throw new Error('Goal objective must not be empty.');
  if ([...objective].length > 4000) throw new Error('Goal objective must be at most 4000 characters. Use a referenced file for longer objectives.');
  return objective;
}

export function validateBudget(value, maximum) {
  if (value !== null && (!Number.isSafeInteger(value) || value <= 0)) throw new Error('Goal budgets must be positive integers.');
  if (maximum && (value === null || value > maximum)) throw new Error(`Goal token budget exceeds the configured maximum of ${maximum}.`);
  return value;
}

// Pi reports input and cache reads separately, unlike Codex's inclusive input.
// Cache writes are fresh input. Output already includes reported reasoning tokens.
export function goalTokens(usage) {
  return integer(usage?.input) + integer(usage?.cacheWrite) + integer(usage?.output);
}

export function goalPrompt(kind, goal, planTool) {
  let template = templates[kind];
  if (kind === 'continuation') {
    if (!planTool) template = template.replace(/Progress visibility:\n[\s\S]*?\n\nFidelity:/, 'Fidelity:');
    else template = template.replaceAll('update_plan', planTool);
  }
  const values = {
    objective: goal.objective.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;'),
    tokens_used: String(goal.tokensUsed),
    time_used_seconds: String(goal.timeUsedSeconds),
    token_budget: goal.tokenBudget === null ? 'none' : String(goal.tokenBudget),
    remaining_tokens: goal.tokenBudget === null ? (kind === 'objective_updated' ? 'unknown' : 'unbounded')
      : String(Math.max(0, goal.tokenBudget - goal.tokensUsed)),
  };
  return template.replace(/{{ (\w+) }}/g, (_, key) => values[key]);
}

export function parseGoalCommand(raw) {
  const text = raw.trim();
  if (!text) return { action: 'status', rest: '' };
  const match = /^(set|edit|status|show|pause|resume|clear|budget|help)(?:\s+([\s\S]*))?$/i.exec(text);
  return match ? { action: match[1].toLowerCase(), rest: match[2] ?? '' } : { action: 'set', rest: text };
}

function normalizeGoal(value, version) {
  if (!value) return null;
  try {
    const objective = validateObjective(value.objective);
    const legacy = version !== 3;
    const status = legacy ? (value.status === 'complete' ? 'complete' : 'paused')
      : STATUSES.includes(value.status) ? value.status : 'paused';
    return {
      id: value.id || randomUUID(), revision: Math.max(1, integer(value.revision)), objective, status,
      tokenBudget: legacy || value.tokenBudget == null ? null : validateBudget(value.tokenBudget),
      tokensUsed: integer(value.tokensUsed), timeUsedSeconds: integer(value.timeUsedSeconds),
      createdAt: integer(value.createdAt) || Date.now(), updatedAt: integer(value.updatedAt) || Date.now(),
      deferred: Boolean(value.deferred),
      reason: legacy ? 'Imported from the retired auditor workflow. Usage starts from migration; unfinished goals require /goal resume.' : String(value.reason ?? ''),
    };
  } catch { return null; }
}

function summary(goal) {
  if (!goal) return `No goal is set.\n${HELP}`;
  return [`Goal: ${goal.status}`, goal.objective,
    `Tokens: ${goal.tokensUsed}/${goal.tokenBudget ?? 'unbounded'}; elapsed: ${goal.timeUsedSeconds}s`,
    goal.reason, HELP].filter(Boolean).join('\n\n');
}

const objectSchema = properties => ({ type: 'object', properties, required: Object.keys(properties).filter(key => key !== 'token_budget'), additionalProperties: false });
const descriptions = {
  get_goal: 'Get the current goal for this session, including status, budgets, token and elapsed-time usage, and remaining token budget.',
  create_goal: 'Create a goal only when explicitly requested by the user or system/developer instructions; do not infer goals from ordinary tasks. Set token_budget only when an explicit token budget is requested. Fails if an unfinished goal exists; use update_goal only for status.',
  update_goal: `Update the existing goal. Set status to paused only at the user's explicit request to pause this goal, never on your own initiative. Ask if unclear; a later resume revokes that request. Report the returned status and stop goal work. Budget limits take precedence over pausing.
Set complete only when the objective has actually been achieved and no required work remains. Do not mark complete merely because its budget is nearly exhausted or you are stopping work.
Set blocked only when the same blocking condition has repeated for at least three consecutive goal turns, counting the original/user-triggered turn and automatic continuations, and you cannot make meaningful progress without user input or an external-state change. After a blocked goal is resumed, the resumed run starts a fresh blocked audit. Once the threshold is satisfied, mark blocked instead of repeatedly reporting it while leaving the goal active. Do not use blocked merely because work is hard, slow, uncertain, incomplete, or would benefit from clarification.
You cannot resume, budget-limit, usage-limit, edit the objective or change budgets with this tool. Those changes belong to the user or system. When completing a budgeted goal, report final token usage from the tool result.`,
};

export function createGoalExtension({ now = () => Date.now(), clock = () => performance.now(), maxTokenBudget = null } = {}) {
  return function goalExtension(pi) {
    let goal = null, epoch = 0, turn = null, queued = null, ended = null;
    let interrupted = false, emptyTurns = 0, lastClock = clock();
    const messagesSeen = new WeakSet();
    const toolsSeen = new Set();
    const childUsage = new Map();
    const shellCalls = new Set();
    let executionFailures = 0;

    pi.registerFlag('max-goal-token-budget', { description: 'Maximum uncached-input plus output tokens per goal', type: 'string' });
    const maximum = () => {
      const flag = pi.getFlag?.('max-goal-token-budget');
      const value = flag === undefined ? maxTokenBudget : Number(flag);
      return value == null ? null : validateBudget(value);
    };
    const persistent = ctx => Boolean(ctx.sessionManager.getSessionFile());
    const owned = () => goal && turn?.id === goal.id && turn?.epoch === epoch;
    const planTool = () => pi.getActiveTools?.().find(name => name === 'update_plan')
      ?? pi.getActiveTools?.().find(name => name === 'todo');
    const notify = (ctx, text, level = 'info') => { if (ctx.hasUI) ctx.ui.notify(text, level); };
    function display(ctx) {
      if (ctx.hasUI) ctx.ui.setStatus('goal', goal
        ? `Goal ${goal.status}${interrupted ? ' (interrupted)' : ''} · ${goal.tokensUsed}/${goal.tokenBudget ?? '∞'} tokens · ${goal.timeUsedSeconds}s`
        : undefined);
    }
    function persist(action, ctx) {
      if (goal) goal.updatedAt = now();
      pi.appendEntry(STATE_TYPE, { version: 3, action, goal: goal ? structuredClone(goal) : null });
      display(ctx);
    }
    function invalidate() {
      epoch++; queued = null; ended = null; turn = null;
      emptyTurns = 0; executionFailures = 0; shellCalls.clear();
      lastClock = clock();
    }
    function contextMessage(kind) {
      return { customType: CONTEXT_TYPE, content: goalPrompt(kind, goal, planTool()), display: false,
        details: { id: goal.id, revision: goal.revision, epoch, kind } };
    }
    function steer(kind, ctx) {
      if (!goal || ctx.isIdle()) return;
      pi.sendMessage(contextMessage(kind), { deliverAs: 'steer' });
    }
    function account(ctx, tokens = 0) {
      if (!owned() || !['active', 'budget_limited'].includes(goal.status)) return;
      const seconds = Math.max(0, Math.floor((clock() - lastClock) / 1000));
      if (!tokens && !seconds) return;
      lastClock += seconds * 1000;
      goal.tokensUsed += tokens;
      goal.timeUsedSeconds += seconds;
      const exhausted = goal.status === 'active' && goal.tokenBudget !== null && goal.tokensUsed >= goal.tokenBudget;
      if (exhausted) { goal.status = 'budget_limited'; goal.reason = 'Token budget reached. Increase the budget before resuming.'; }
      persist('usage', ctx);
      if (exhausted) steer('budget_limit', ctx);
    }
    function setStatus(status, ctx) {
      account(ctx);
      if (goal.status === 'budget_limited' && ['paused', 'blocked'].includes(status)) status = 'budget_limited';
      if (status === 'active' && goal.tokenBudget !== null && goal.tokensUsed >= goal.tokenBudget) status = 'budget_limited';
      invalidate();
      goal.status = status; goal.reason = ''; goal.deferred = false; interrupted = false;
      if (status === 'active' && !ctx.isIdle()) turn = { id: goal.id, epoch, automatic: false, activity: false, successfulTool: false, failedExecution: false };
      persist('status', ctx);
    }
    function response(ctx, completed = false) {
      const value = {
        goal: goal ? { threadId: ctx.sessionManager.getSessionId(), objective: goal.objective, status: goal.status,
          tokenBudget: goal.tokenBudget, tokensUsed: goal.tokensUsed, timeUsedSeconds: goal.timeUsedSeconds,
          createdAt: Math.floor(goal.createdAt / 1000), updatedAt: Math.floor(goal.updatedAt / 1000) } : null,
        remainingTokens: goal?.tokenBudget != null ? Math.max(0, goal.tokenBudget - goal.tokensUsed) : null,
        completionBudgetReport: completed && goal?.status === 'complete' && (goal.tokenBudget !== null || goal.timeUsedSeconds > 0)
          ? 'Goal achieved. Report final token usage and budget, when present, and elapsed time from the structured goal fields.' : null,
      };
      return { content: [{ type: 'text', text: JSON.stringify(value) }], details: value };
    }
    function start(objective, budget, ctx) {
      account(ctx); invalidate(); childUsage.clear(); toolsSeen.clear();
      goal = { id: randomUUID(), revision: 1, objective, status: 'active', tokenBudget: budget,
        tokensUsed: 0, timeUsedSeconds: 0, createdAt: now(), updatedAt: now(), reason: '', deferred: false };
      interrupted = false;
      // create_goal can be called during an existing run: exclude its prior usage.
      if (!ctx.isIdle()) turn = { id: goal.id, epoch, automatic: false, activity: false, successfulTool: false, failedExecution: false };
      persist('set', ctx);
    }
    function queueContinuation(ctx) {
      if (!goal || goal.status !== 'active' || goal.deferred || interrupted || queued || !persistent(ctx)
        || !ctx.isIdle() || ctx.hasPendingMessages()) return;
      if (!pi.getActiveTools?.().includes('update_goal')) {
        notify(ctx, 'Goal continuation requires the update_goal tool to be enabled.', 'warning');
        return;
      }
      // There is no await in this admission window. Pi owns retries and pending input.
      queued = { id: goal.id, epoch };
      try { pi.sendMessage(contextMessage('continuation'), { triggerTurn: true }); }
      catch (error) {
        queued = null; interrupted = true;
        notify(ctx, `Goal continuation could not start: ${error.message}`, 'warning'); display(ctx);
      }
    }
    function restore(event, ctx) {
      invalidate(); childUsage.clear(); toolsSeen.clear(); interrupted = false; goal = null;
      for (const entry of ctx.sessionManager.getBranch()) {
        if (entry.type === 'custom' && entry.customType === STATE_TYPE) goal = normalizeGoal(entry.data?.goal, entry.data?.version);
      }
      if (goal && (event.reason === 'fork' || event.type === 'session_tree')) {
        goal.deferred = true;
        persist('fork', ctx);
      }
      display(ctx);
      // Never charge time spent with the application closed.
      const owner = epoch;
      setTimeout(() => { if (epoch === owner) queueContinuation(ctx); }, 0).unref?.();
    }

    for (const name of Object.keys(descriptions)) {
      const parameters = objectSchema(name === 'create_goal' ? {
        objective: { type: 'string', description: 'Concrete objective to pursue.' },
        token_budget: { type: 'integer', minimum: 1, description: 'Omit unless explicitly requested.' },
      } : name === 'update_goal' ? {
        status: { type: 'string', enum: ['complete', 'blocked', 'paused'] },
      } : {});
      pi.registerTool({ name, label: name.replaceAll('_', ' '), description: descriptions[name], parameters,
        async execute(_callId, args, signal, _onUpdate, ctx) {
          if (signal?.aborted) throw new Error('Goal operation cancelled.');
          if (!persistent(ctx)) throw new Error('Goal tools require a persistent session.');
          if (name === 'get_goal') return response(ctx);
          if (name === 'create_goal') {
            const objective = validateObjective(args.objective);
            const budget = validateBudget(args.token_budget ?? maximum(), maximum());
            if (goal && goal.status !== 'complete') throw new Error('Cannot create a goal while an unfinished goal exists.');
            start(objective, budget, ctx);
          } else {
            if (!['complete', 'blocked', 'paused'].includes(args.status)) throw new Error('Only complete, blocked and paused are agent-controlled statuses.');
            if (!goal) throw new Error('No goal exists.');
            if (turn && !owned()) throw new Error('Goal changed during this run. Read the current goal before updating it.');
            setStatus(args.status, ctx);
          }
          return response(ctx, name === 'update_goal' && args.status === 'complete');
        },
      });
    }

    pi.on('session_start', restore);
    pi.on('session_tree', (event, ctx) => restore({ ...event, type: 'session_tree' }, ctx));
    pi.on('session_shutdown', (_event, ctx) => { account(ctx); invalidate(); });
    pi.on('input', (event, ctx) => {
      if (event.source !== 'extension') {
        interrupted = false; emptyTurns = 0;
        if (goal?.deferred) { goal.deferred = false; persist('undefer', ctx); }
      }
    });
    pi.on('agent_start', (_event, ctx) => {
      const automatic = queued?.id === goal?.id && queued?.epoch === epoch;
      queued = null; ended = null;
      turn = goal && ['active', 'budget_limited'].includes(goal.status)
        ? { id: goal.id, epoch, automatic, activity: false, successfulTool: false, failedExecution: false } : null;
      lastClock = clock();
    });
    pi.on('message_end', (event, ctx) => {
      const message = event.message;
      if (message.role !== 'assistant' || messagesSeen.has(message)) return;
      messagesSeen.add(message);
      if (owned()) {
        turn.activity ||= message.content?.some(part => part.type === 'toolCall' || Boolean((part.text ?? part.thinking ?? '').trim()));
        account(ctx, goalTokens(message.usage));
      }
    });
    pi.on('tool_call', event => {
      if (event.toolName === 'bash') shellCalls.add(event.toolCallId);
    });
    pi.on('tool_result', (event, ctx) => {
      if (!owned() || toolsSeen.has(event.toolCallId)) return;
      toolsSeen.add(event.toolCallId); turn.activity = true;
      if (!event.isError) { turn.successfulTool = true; executionFailures = 0; }
      // Pi conflates thrown shell-handler errors with command exit failures. Only
      // explicit spawn errors prove execution was unavailable, never isError alone.
      if (event.isError && shellCalls.has(event.toolCallId) && event.content?.some(part =>
        part.type === 'text' && /\bspawn\b[^\n]*\b(?:ENOENT|EACCES|ENOEXEC)\b/.test(part.text))) turn.failedExecution = true;
      let tokens = goalTokens(event.usage);
      if (!event.usage && ['team', 'subagent'].includes(event.toolName)) {
        // Team polling returns cumulative child snapshots. Charge each child once,
        // not again on read/wait. Snapshot accounting cannot see unreported live work.
        for (const result of event.details?.results ?? []) {
          const key = result.sessionId ?? result.sessionFile ?? result.memberId;
          const total = goalTokens(result.usage);
          if (!key) continue;
          const previous = childUsage.get(key) ?? 0;
          tokens += Math.max(0, total - previous);
          childUsage.set(key, Math.max(previous, total));
        }
      }
      account(ctx, tokens);
    });
    pi.on('agent_end', (event, ctx) => {
      if (!owned()) return;
      account(ctx);
      const last = [...(event.messages ?? [])].reverse().find(message => message.role === 'assistant');
      ended = { ...turn, last };
      // Host compaction/retry may follow; only agent_settled decides terminal errors.
    });
    pi.on('agent_settled', (_event, ctx) => {
      const result = ended; ended = null;
      if (result && result.id === goal?.id && result.epoch === epoch) {
        if (result.last?.stopReason === 'aborted') {
          interrupted = true; emptyTurns = 0; display(ctx); return;
        }
        if (result.last?.stopReason === 'error') {
          const error = result.last.errorMessage ?? '';
          // Rate-limit retries belong to Pi. Only a final explicit quota/usage-limit
          // failure gets this status; other unrecovered errors block the goal.
          setStatus(/usage limit|quota (?:exceeded|exhausted)|insufficient_quota/i.test(error) ? 'usage_limited' : 'blocked', ctx);
          goal.reason = 'Turn failed after host recovery. Inspect the error before resuming.';
          persist('error', ctx); return;
        }
        if (result.failedExecution && !result.successfulTool) executionFailures++;
        const empty = result.automatic && result.last && !result.activity
          && !result.last.content?.some(part => Boolean((part.text ?? part.thinking ?? '').trim()));
        emptyTurns = empty ? emptyTurns + 1 : 0;
        if (emptyTurns >= 3 || executionFailures >= 3) {
          const reason = emptyTurns >= 3 ? 'Three empty automatic goal responses.' : 'Repeated execution was unavailable.';
          setStatus('blocked', ctx); goal.reason = reason; persist('blocked', ctx); return;
        }
      }
      turn = null;
      queueContinuation(ctx);
    });
    pi.on('context', event => {
      // Keep only the newest current internal goal message. Old contexts and retired
      // marker continuations must not revive a cleared or replaced objective.
      const valid = message => goal && message.details?.id === goal.id && message.details?.revision === goal.revision
        && message.details?.epoch === epoch && ['active', 'budget_limited'].includes(goal.status);
      const last = event.messages.findLastIndex(message => message.customType === CONTEXT_TYPE && valid(message));
      return { messages: event.messages.filter((message, index) => message.customType !== 'goal-continuation'
        && (message.customType !== CONTEXT_TYPE || index === last)) };
    });

    pi.registerCommand('goal', {
      description: 'Manage a persistent goal with Codex-style continuation and usage accounting',
      getArgumentCompletions(prefix) {
        return ['set', 'edit', 'status', 'pause', 'resume', 'budget', 'clear', 'help'].filter(value => value.startsWith(prefix.trimStart()))
          .map(value => ({ value, label: value }));
      },
      async handler(raw, ctx) {
        const { action, rest } = parseGoalCommand(raw);
        try {
          if (!['set', 'edit', 'budget'].includes(action) && rest) throw new Error(`Unexpected arguments. ${HELP}`);
          if (action === 'help') { notify(ctx, HELP); return; }
          if (['status', 'show'].includes(action)) { account(ctx); notify(ctx, summary(goal)); display(ctx); return; }
          if (!persistent(ctx)) throw new Error('Goals require a persistent session.');
          if (action === 'clear') { account(ctx); invalidate(); goal = null; childUsage.clear(); persist('clear', ctx); return; }
          if (action !== 'set' && !goal) throw new Error('No goal exists.');
          if (action === 'pause' || action === 'resume') {
            setStatus(action === 'pause' ? 'paused' : 'active', ctx);
            notify(ctx, summary(goal));
            if (action === 'resume') queueContinuation(ctx);
            return;
          }
          if (action === 'budget') {
            const budget = validateBudget(rest === 'none' ? maximum() : Number(rest), maximum());
            account(ctx); goal.tokenBudget = budget;
            if (goal.status === 'active' && budget !== null && goal.tokensUsed >= budget) {
              goal.status = 'budget_limited'; steer('budget_limit', ctx);
            }
            persist('budget', ctx); notify(ctx, summary(goal)); return;
          }
          let objective = rest;
          const owner = epoch;
          if (action === 'edit' && !objective && ctx.hasUI) objective = await ctx.ui.editor('Edit goal:', goal.objective);
          if (objective === undefined) return;
          objective = validateObjective(objective);
          if (action === 'set' && goal && goal.status !== 'complete') {
            if (!ctx.hasUI) throw new Error('Clear the unfinished goal explicitly before replacing it.');
            if (!await ctx.ui.confirm('Replace goal?', objective)) return;
          }
          if (owner !== epoch) throw new Error('Goal changed while the dialog was open. Retry the command.');
          if (action === 'edit') {
            account(ctx);
            const wasRunning = !ctx.isIdle();
            invalidate(); goal.objective = objective; goal.revision++;
            if (wasRunning && goal.status === 'active') turn = { id: goal.id, epoch, automatic: false, activity: true, successfulTool: false, failedExecution: false };
            persist('edit', ctx);
            if (goal.status === 'active') steer('objective_updated', ctx);
          } else start(objective, maximum(), ctx);
          notify(ctx, summary(goal)); queueContinuation(ctx);
        } catch (error) { notify(ctx, error.message, 'error'); }
      },
    });
  };
}

export default createGoalExtension();
