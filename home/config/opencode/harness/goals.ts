import { randomUUID } from "node:crypto";
import { performance } from "node:perf_hooks";
import { descriptions, goalPrompt, parseGoalCommand, validateBudget, validateObjective } from "./goal-source/index.ts";

const HELP = "/goal <objective> | set <objective> | edit [objective] | status | pause | resume | budget <tokens|none> | clear";
const number = value => Number.isFinite(value) && value >= 0 ? value : 0;
// V2 separates reasoning from visible output and cache writes from fresh input.
export const goalTokens = tokens => number(tokens?.input) + number(tokens?.output)
  + number(tokens?.reasoning) + number(tokens?.cache?.write);

export function goalSummary(goal) {
  return goal ? [`Goal: ${goal.status}${goal.interrupted ? " (interrupted)" : ""}`, goal.objective,
    `Tokens: ${goal.tokensUsed}/${goal.tokenBudget ?? "unbounded"}; elapsed: ${goal.timeUsedSeconds}s`,
    goal.reason, HELP].filter(Boolean).join("\n\n") : `No goal is set.\n${HELP}`;
}

export async function setupGoals(ctx, rpc) {
  const maximum = ctx.options.maxGoalTokenBudget == null ? null : validateBudget(ctx.options.maxGoalTokenBudget);
  const states = new Map();
  const locks = new Map();
  const clocks = new Map();
  const requests = new Map();
  const lock = (id, work) => {
    const next = (locks.get(id) ?? Promise.resolve()).catch(() => {}).then(work);
    locks.set(id, next);
    return next.finally(() => { if (locks.get(id) === next) locks.delete(id); });
  };
  const key = id => `goal/${id}`;
  async function load(id) {
    if (!states.has(id)) states.set(id, await ctx.storage.get(key(id)) ?? { goal: null, accounted: {}, closing: false, automatic: false, emptyTurns: 0, executionFailures: 0 });
    return states.get(id);
  }
  const view = (id, state) => ({ sessionID: id, goal: state.goal, text: goalSummary(state.goal) });
  let registration;
  async function save(id, state) {
    if (state.goal) state.goal.updatedAt = Date.now();
    await ctx.storage.set(key(id), structuredClone(state));
    await registration?.events.emit("goal", view(id, state));
  }
  async function snapshots(id) {
    const sessions = [await ctx.session.get({ sessionID: id })];
    for (let at = 0; at < sessions.length; at++) {
      let cursor;
      do {
        const page = await ctx.session.list(cursor ? { cursor } : { parentID: sessions[at].id, limit: 100 });
        sessions.push(...page.data);
        cursor = page.cursor.next;
      } while (cursor);
    }
    return Object.fromEntries(sessions.map(session => [session.id, goalTokens(session.tokens)]));
  }
  async function account(id, state) {
    if (!state.goal || (!state.closing && !["active", "budget_limited"].includes(state.goal.status))) return;
    const current = await snapshots(id);
    for (const [child, total] of Object.entries(current)) {
      state.goal.tokensUsed += Math.max(0, total - (state.accounted[child] ?? 0));
      state.accounted[child] = Math.max(total, state.accounted[child] ?? 0);
    }
    const previous = clocks.get(id);
    if (previous !== undefined) {
      const seconds = Math.max(0, Math.floor((performance.now() - previous) / 1000));
      state.goal.timeUsedSeconds += seconds;
      clocks.set(id, previous + seconds * 1000);
    }
    if (state.goal.status === "active" && state.goal.tokenBudget !== null
      && state.goal.tokensUsed >= state.goal.tokenBudget) {
      state.goal.status = "budget_limited";
      state.goal.reason = "Token budget reached. Increase the budget before resuming.";
    }
    await save(id, state);
  }
  async function resume(id, state) {
    state.accounted = await snapshots(id);
    state.closing = false;
    state.automatic = false;
    state.emptyTurns = 0;
    state.executionFailures = 0;
    state.goal.interrupted = false;
    state.goal.reason = "";
    state.goal.status = state.goal.tokenBudget !== null && state.goal.tokensUsed >= state.goal.tokenBudget
      ? "budget_limited" : "active";
    await save(id, state);
    if (state.goal.status === "active") await ctx.session.synthetic({ sessionID: id,
      text: "Resume the explicitly requested goal. Read get_goal and pursue its full objective.", metadata: { goal: state.goal.id } });
  }
  async function start(id, state, objective, budget) {
    const now = Date.now();
    state.accounted = await snapshots(id);
    state.closing = false;
    state.automatic = false;
    state.emptyTurns = 0;
    state.executionFailures = 0;
    state.goal = { id: randomUUID(), revision: 1, objective: validateObjective(objective),
      status: "active", tokenBudget: validateBudget(budget ?? maximum, maximum), tokensUsed: 0, timeUsedSeconds: 0,
      createdAt: now, updatedAt: now, interrupted: false, reason: "" };
    clocks.set(id, performance.now());
    await save(id, state);
  }
  const toolResult = state => ({ goal: state.goal,
    remainingTokens: state.goal?.tokenBudget == null ? null : Math.max(0, state.goal.tokenBudget - state.goal.tokensUsed) });

  async function command({ sessionID: id, command: raw, replace = false }) {
    return lock(id, async () => {
      const state = await load(id);
      await account(id, state);
      const { action, rest } = parseGoalCommand(raw);
      if (!["set", "edit", "budget"].includes(action) && rest) throw new Error(`Unexpected arguments. ${HELP}`);
      if (action === "help" || ["status", "show"].includes(action)) return view(id, state);
      if (action !== "set" && action !== "clear" && !state.goal) throw new Error("No goal exists.");
      state.epoch = (state.epoch ?? 0) + 1;
      state.controlledAt = Date.now();
      if (action === "clear") {
        await ctx.session.interrupt({ sessionID: id, resume: false });
        state.goal = null;
        state.accounted = {};
        state.closing = false;
        clocks.delete(id);
      } else if (action === "set") {
        if (state.goal && state.goal.status !== "complete" && !replace) throw new Error("Confirm replacement of the unfinished goal first.");
        await start(id, state, rest, null);
        await ctx.session.synthetic({ sessionID: id, text: "Pursue the explicitly requested goal. Read get_goal before starting.", metadata: { goal: state.goal.id } });
      } else if (action === "pause") {
        await ctx.session.interrupt({ sessionID: id, resume: false });
        if (state.goal.status !== "budget_limited") state.goal.status = "paused";
        state.goal.interrupted = false;
        clocks.delete(id);
      } else if (action === "resume") {
        await resume(id, state);
      } else if (action === "budget") {
        state.goal.tokenBudget = validateBudget(rest === "none" ? maximum : Number(rest), maximum);
        if (state.goal.status === "active" && state.goal.tokenBudget !== null && state.goal.tokensUsed >= state.goal.tokenBudget) {
          state.goal.status = "budget_limited";
          state.goal.reason = "Token budget reached. Increase the budget before resuming.";
        }
      } else if (action === "edit") {
        state.goal.objective = validateObjective(rest);
        state.goal.revision++;
      }
      await save(id, state);
      return view(id, state);
    });
  }
  registration = await ctx.rpc.register(rpc, { goal: command });
  await ctx.command.transform(editor => editor.add({
    name: "goal", description: "Manage a persistent goal, continuation and token budget",
    async execute({ sessionID, prompt }) { await command({ sessionID, command: prompt.text }); },
  }));
  await ctx.tool.transform(editor => {
    for (const [name, description] of Object.entries(descriptions)) editor.add({
      name, description, options: { codemode: false },
      input: { type: "object", properties: name === "create_goal" ? {
        objective: { type: "string" }, token_budget: { type: "integer", minimum: 1 },
      } : name === "update_goal" ? { status: { type: "string", enum: ["complete", "blocked", "paused"] } } : {},
      required: name === "create_goal" ? ["objective"] : name === "update_goal" ? ["status"] : [], additionalProperties: false },
      async execute(input, context) {
        await ctx.permission.assert({ action: name, resources: ["*"], sessionID: context.sessionID, agent: context.agent });
        return lock(context.sessionID, async () => {
          const state = await load(context.sessionID);
          await account(context.sessionID, state);
          if (name === "create_goal") {
            if (state.goal && state.goal.status !== "complete") throw new Error("An unfinished goal already exists.");
            await start(context.sessionID, state, input.objective, input.token_budget ?? null);
            requests.set(context.sessionID, { id: state.goal.id, revision: state.goal.revision, epoch: state.epoch });
          } else if (name === "update_goal") {
            if (!state.goal) throw new Error("No goal exists.");
            const request = requests.get(context.sessionID);
            if (!request || request.id !== state.goal.id || request.revision !== state.goal.revision || request.epoch !== state.epoch) {
              throw new Error("Goal changed during this request. Read the current goal before updating it in a subsequent step.");
            }
            if (!['complete', 'blocked', 'paused'].includes(input.status)) throw new Error("Invalid goal status.");
            state.goal.status = state.goal.status === "budget_limited" && input.status !== "complete" ? "budget_limited" : input.status;
            state.closing = input.status === "complete";
            await save(context.sessionID, state);
          }
          const result = toolResult(state);
          return { content: JSON.stringify(result) };
        });
      },
    });
  });
  await ctx.session.hook("prompt", event => lock(event.sessionID, async () => {
    const state = await load(event.sessionID);
    state.automatic = false;
    state.emptyTurns = 0;
    if (state.goal?.interrupted && state.goal.status === "active") {
      state.goal.interrupted = false;
      await save(event.sessionID, state);
    }
  }));
  await ctx.session.hook("context", event => lock(event.sessionID, async () => {
    const state = await load(event.sessionID);
    requests.set(event.sessionID, state.goal ? { id: state.goal.id, revision: state.goal.revision, epoch: state.epoch } : null);
    if (!state.goal) return;
    if (!clocks.has(event.sessionID)) clocks.set(event.sessionID, performance.now());
    await account(event.sessionID, state);
    if (!["active", "budget_limited"].includes(state.goal.status)) return;
    const planTool = Object.keys(event.tools).find(name => ["update_plan", "todo", "todowrite"].includes(name));
    // Ephemeral user context keeps only the current goal snapshot on each request.
    event.messages.push({ role: "user", content: [{ type: "text",
      text: goalPrompt(state.goal.status === "budget_limited" ? "budget_limit" : "continuation", state.goal, planTool) }] });
  }));
  await ctx.session.hook("experimental.idle", event => lock(event.sessionID, async () => {
    const state = await load(event.sessionID);
    await account(event.sessionID, state);
    clocks.delete(event.sessionID);
    if (state.closing) { state.closing = false; await save(event.sessionID, state); }
    if (!state.goal || state.goal.status !== "active" || state.goal.interrupted || event.background.length) return;
    const messages = await ctx.session.context({ sessionID: event.sessionID });
    const execution = messages.slice(messages.findLastIndex(message => message.type === "idle") + 1);
    const responses = execution.filter(message => message.type === "assistant");
    const last = responses.at(-1);
    const useful = last?.content?.some(part => part.type === "tool" || Boolean(part.text?.trim()));
    state.emptyTurns = state.automatic && !useful ? state.emptyTurns + 1 : 0;
    const tools = responses.flatMap(message => message.content ?? []).filter(part => part.type === "tool");
    const unavailable = tools.some(part => part.name === "shell" && part.state.status === "error"
      && /\bspawn\b[^\n]*\b(?:ENOENT|EACCES|ENOEXEC)\b/.test(part.state.error?.message ?? ""));
    state.executionFailures = tools.some(part => part.state.status === "completed") ? 0
      : unavailable ? state.executionFailures + 1 : state.executionFailures;
    if (state.emptyTurns >= 3 || state.executionFailures >= 3) {
      state.goal.status = "blocked";
      state.goal.reason = state.emptyTurns >= 3 ? "Three empty automatic goal responses." : "Repeated execution was unavailable.";
      await save(event.sessionID, state);
      return;
    }
    state.automatic = true;
    await save(event.sessionID, state);
    await ctx.session.synthetic({ sessionID: event.sessionID,
      text: `Continue only if get_goal still identifies active goal ${state.goal.id}, revision ${state.goal.revision}. Otherwise ignore this stale continuation.`,
      metadata: { goal: state.goal.id, revision: state.goal.revision } });
  }));
  const controller = new AbortController();
  const types = ["session.execution.interrupted", "session.execution.failed", "session.deleted"];
  const events = (async () => {
    for await (const event of ctx.event.subscribe({ types, signal: controller.signal })) {
      const id = event.data.sessionID;
      await lock(id, async () => {
        if (event.type === "session.deleted") {
          states.delete(id); clocks.delete(id); requests.delete(id); await ctx.storage.remove(key(id)); return;
        }
        const state = await load(id);
        await account(id, state);
        clocks.delete(id);
        if (!state.goal || state.goal.status !== "active") return;
        if (event.created < (state.controlledAt ?? state.goal.createdAt)) return;
        if (event.type === "session.execution.interrupted") {
          state.goal.interrupted = true;
          state.epoch = (state.epoch ?? 0) + 1;
        }
        else {
          const error = JSON.stringify(event.data.error);
          state.goal.status = /usage limit|quota (?:exceeded|exhausted)|insufficient_quota/i.test(error) ? "usage_limited" : "blocked";
          state.goal.reason = "Turn failed after host recovery. Inspect the error before resuming.";
        }
        await save(id, state);
      });
    }
  })();
  events.catch(error => { if (!controller.signal.aborted) console.error("Goal event subscription failed", error); });
  return () => controller.abort();
}
