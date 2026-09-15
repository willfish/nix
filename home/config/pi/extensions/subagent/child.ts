import { randomUUID } from 'node:crypto';
import { readFile, readdir, rename, rm, stat } from 'node:fs/promises';
import { join, isAbsolute } from 'node:path';
import { atomicJson, readJson, envelope, assistantResult, VERSION } from './protocol.ts';
import { socketCall } from './herdr.ts';

/** Explicitly loaded only for interactive team children. No terminal scraping. */
export default function childExtension(pi, options = {}) {
  pi.registerFlag('team-run', { description: 'Private coordinator IPC directory', type: 'string' });
  let dir, request, ctx, timer, processing = false, stopped = false, active;
  let messages = [], question, batch = [], segmentAborted = false;
  const seen = new Set();
  const questionTool = 'ask_coordinator';
  const syncTools = () => {
    const tools = pi.getActiveTools().filter((name) => name !== questionTool);
    if (dir && !stopped && active && !question && !segmentAborted) tools.push(questionTool);
    pi.setActiveTools(tools);
  };
  const questionError = (context) => {
    if (!dir || stopped || !active || context.mode !== 'tui') return 'Questions require an active interactive team command';
    if (question) return 'A coordinator question is already pending';
    if (segmentAborted || context.signal?.aborted) return 'Team command was cancelled';
    if (batch.length !== 1 || batch[0].name !== questionTool) return 'ask_coordinator must be the only tool call in its assistant message';
    // ExtensionContext exposes this query, not a queue-clearing API. Refuse to
    // pause until existing steering/follow-ups have drained through Pi.
    if (context.hasPendingMessages()) return 'Process pending steering/follow-up messages before asking the coordinator';
  };
  const registerQuestionTool = () => pi.registerTool({
    name: questionTool, label: 'Ask coordinator',
    description: 'Pause this team task for a coordinator answer. Call alone, with no sibling tools. Set requiresUser for a human decision.',
    parameters: {
      type: 'object', additionalProperties: false, required: ['text'],
      properties: {
        text: { type: 'string', minLength: 1 },
        choices: { type: 'array', items: { type: 'string', minLength: 1 }, minItems: 1 },
        requiresUser: { type: 'boolean' },
      },
    },
    async execute(_id, params, signal, _onUpdate, context) {
      const error = questionError(context);
      if (error) throw new Error(error);
      if (signal?.aborted) throw new Error('Team command was cancelled');
      if (typeof params.text !== 'string' || !params.text.trim() ||
          (params.requiresUser !== undefined && typeof params.requiresUser !== 'boolean') ||
          (params.choices !== undefined && (!Array.isArray(params.choices) || !params.choices.length ||
            params.choices.some((choice) => typeof choice !== 'string' || !choice.trim())))) {
        throw new Error('Invalid coordinator question');
      }
      // Set the pause synchronously before any I/O, so poll/input cannot queue
      // steering between the pending-message check and the terminating result.
      question = { id: randomUUID(), text: params.text,
        ...(params.choices === undefined ? {} : { choices: [...params.choices] }),
        requiresUser: params.requiresUser ?? false, commandId: active };
      syncTools();
      return { content: [{ type: 'text', text: 'Waiting for coordinator answer.' }], details: { question }, terminate: true };
    },
  });
  const every = options.every ?? setInterval;
  const cancel = options.cancel ?? clearInterval;
  let writes = Promise.resolve();
  const write = (name, value) => {
    const next = writes.then(() => (options.writeJson ?? atomicJson)(join(dir, name),
      envelope(request.runId, typeof value === 'function' ? value() : value)));
    writes = next.catch(() => {});
    return next;
  };
  // Evaluate state inside the write queue; a delayed poll must not overwrite newer settlement state.
  const status = () => write('status.json', () => ({
    pid: process.pid, idle: ctx.isIdle() && !active && !question, commandId: active, question,
    sessionId: ctx.sessionManager.getSessionId(), sessionFile: ctx.sessionManager.getSessionFile(),
    updated: Date.now(), stopped,
  }));
  const result = async (id, value) => {
    const data = { ...value, commandId: id, updated: Date.now(),
      sessionId: ctx.sessionManager.getSessionId(), sessionFile: ctx.sessionManager.getSessionFile() };
    await write('last.json', data);
    if (id) await write(`results/${id}.json`, data);
  };
  const fail = async (error) => {
    stopped = true;
    if (timer) cancel(timer);
    if (active) await result(active, { status: 'error', text: '', errorMessage: error.message });
    active = undefined;
    question = undefined;
    syncTools();
    await status();
    ctx.ui.notify(`Team bridge stopped: ${error.message}`, 'error');
  };
  const poll = async () => {
    if (!dir || stopped || processing) return;
    processing = true;
    try {
      const lease = await readJson(join(dir, 'lease.json'), request.runId);
      if (!lease || Date.now() - lease.updated > (options.leaseMs ?? 30000)) {
        stopped = true;
        cancel(timer);
        if (active) await result(active, { status: 'aborted', text: '', errorMessage: 'Coordinator lease expired' });
        active = undefined;
        question = undefined;
        syncTools();
        await status();
        const terminate = options.terminate ?? (async () => {
          if (request.paneId && request.paneId === process.env.HERDR_PANE_ID) {
            await socketCall(process.env.HERDR_SOCKET_PATH, 'pane.close', { pane_id: request.paneId });
          }
        });
        // Independent deadline: an uncooperative tool must not keep an orphan alive.
        const deadline = setTimeout(() => { void terminate().catch(() => {}); }, 1500);
        deadline.unref?.();
        await ctx.abort();
        ctx.shutdown();
        return;
      }
      const pending = [];
      for (const file of await readdir(join(dir, 'commands'))) {
        if (!/^[a-f0-9-]+\.json$/.test(file)) continue;
        const data = await readJson(join(dir, 'commands', file), request.runId);
        if (data) pending.push({ file, data });
      }
      pending.sort((a, b) => a.data.created - b.data.created || a.file.localeCompare(b.file));
      for (const { file, data } of pending) {
        if (data.id !== file.slice(0, -5)) throw new Error('Team command ID mismatch');
        const claimed = join(dir, `claimed-${file}`);
        await rename(join(dir, 'commands', file), claimed);
        try {
          // Claim even rejected IDs once. A replay must neither inject input nor
          // overwrite the original segment's result, including while it runs.
          if (seen.has(data.id)) continue;
          seen.add(data.id);
          const reject = (errorMessage) => write(`results/${data.id}.json`, {
            status: 'error', text: '', commandId: data.id, errorMessage,
          });
          if (question && !['answer', 'abort'].includes(data.kind)) {
            await reject('Coordinator question pending; answer or cancel it first');
            continue;
          }
          if (data.kind === 'answer') {
            if (!question || question.id !== data.questionId || question.commandId !== data.questionCommandId) {
              await reject('Stale or mismatched coordinator question/segment');
              continue;
            }
            if (!['human', 'coordinator'].includes(data.source) || (question.requiresUser && data.source !== 'human')) {
              await reject('Invalid answer source; this question may require a human answer');
              continue;
            }
            if (active || !ctx.isIdle() || ctx.hasPendingMessages()) {
              await reject('Question segment has not settled');
              continue;
            }
            if (typeof data.text !== 'string' || !data.text.trim()) {
              await reject('Empty team answer');
              continue;
            }
            question = undefined;
            active = data.id;
            messages = [];
            batch = [];
            segmentAborted = false;
            syncTools();
            pi.sendUserMessage(data.text, { deliverAs: 'followUp', expandPromptTemplates: false });
          } else if (data.kind === 'prompt') {
            if (active || !ctx.isIdle()) {
              await result(data.id, { status: 'error', text: '', errorMessage: 'Agent is busy; use steer or wait' });
              continue;
            }
            if (typeof data.text !== 'string' || !data.text.trim()) throw new Error('Empty team prompt');
            active = data.id;
            messages = [];
            batch = [];
            segmentAborted = false;
            syncTools();
            pi.sendUserMessage(data.text, { deliverAs: 'followUp', expandPromptTemplates: false });
          } else if (data.kind === 'steer') {
            if (typeof data.text !== 'string' || !data.text.trim()) throw new Error('Empty team guidance');
            pi.sendUserMessage(data.text, { deliverAs: 'steer', expandPromptTemplates: false });
            await write(`results/${data.id}.json`, { status: 'accepted', commandId: data.id });
          } else if (data.kind === 'retire') {
            if (active || !ctx.isIdle()) {
              await write(`results/${data.id}.json`, { status: 'busy', commandId: data.id });
            } else {
              // Shutdown is requested synchronously before acknowledging retirement.
              stopped = true;
              cancel(timer);
              ctx.shutdown();
              await write(`results/${data.id}.json`, { status: 'retired', commandId: data.id });
              break;
            }
          } else if (data.kind === 'abort') {
            segmentAborted = true;
            const held = question;
            question = undefined;
            syncTools();
            if (held && !active) await result(undefined, { status: 'aborted', text: '', errorMessage: 'Coordinator cancelled pending question' });
            await ctx.abort();
            await write(`results/${data.id}.json`, { status: 'accepted', commandId: data.id });
          } else {
            throw new Error(`Unknown team command: ${data.kind}`);
          }
        } finally { await rm(claimed, { force: true }); }
      }
      await status();
    } catch (error) { await fail(error); }
    finally { processing = false; }
  };

  pi.on('session_start', async (_event, context) => {
    const flag = pi.getFlag('team-run');
    if (!flag) return;
    if (context.mode !== 'tui') throw new Error('Team bridge requires interactive Pi');
    if (dir) return;
    if (typeof flag !== 'string' || !isAbsolute(flag)) throw new Error('Invalid team IPC directory');
    const info = await stat(flag);
    if (!info.isDirectory() || (info.mode & 0o077) || info.uid !== process.getuid?.()) {
      throw new Error('Team IPC directory must be private and owned by this user');
    }
    request = JSON.parse(await readFile(join(flag, 'request.json'), 'utf8'));
    if (request.version !== VERSION || typeof request.runId !== 'string') throw new Error('Invalid team request');
    dir = flag;
    ctx = context;
    stopped = false;
    registerQuestionTool();
    // Never let a child create an uncontrolled recursive team, even via headless delegation.
    pi.setActiveTools(pi.getActiveTools().filter((name) => !['subagent', 'team', questionTool].includes(name)));
    timer = every(() => { void poll(); }, 200);
    timer.unref?.();
    await status();
    await write('ready.json', { pid: process.pid });
  });
  pi.on('before_agent_start', (event) => {
    if (!dir || stopped || !active) return;
    return { systemPrompt: `${event.systemPrompt}\n\nWhen clarification is needed, call ask_coordinator alone instead of ending with an unanswered prose question. The coordinator answers from evidence or asks the human in the main pane. Set requiresUser only for credentials, access, unapproved live/destructive work, or mandatory gates. Do not set it for design, approach, preference, or plan approval. Do not guess approval.` };
  });
  pi.on('input', () => {
    if (!dir || stopped || !question) return;
    ctx.ui.notify('Answer the pending question through the coordinator.', 'warning');
    return { action: 'handled' };
  });
  pi.on('tool_call', (event, context) => {
    if (!dir || stopped) return;
    if (question) return { block: true, reason: 'Coordinator question pending', terminate: true };
    // message_end is drained before ANY sibling preflight, in both sequential
    // and parallel Pi tool modes. Block all siblings, not just the question.
    if (batch.some((call) => call.name === questionTool) && batch.length !== 1) {
      return { block: true, reason: 'ask_coordinator must be the only tool call in its assistant message' };
    }
    if (event.toolName === questionTool) {
      const error = questionError(context);
      if (error) return { block: true, reason: error };
    }
  });
  pi.on('agent_start', async () => {
    if (!dir || stopped) return;
    if (!active && !question) {
      messages = [];
      batch = [];
      segmentAborted = false;
    }
    await status();
  });
  pi.on('message_end', async (event) => {
    if (!dir || stopped || event.message?.role !== 'assistant') return;
    batch = event.message.content?.filter((part) => part.type === 'toolCall') ?? [];
    messages.push(event.message);
    // Tool output remains in the real Pi session. Bound coordinator IPC independently.
    while (Buffer.byteLength(JSON.stringify(messages)) > 6 * 1024 * 1024 && messages.length > 1) messages.shift();
    await write('progress.json', { messages, updated: Date.now() });
  });
  pi.on('agent_settled', async (_event, context) => {
    if (!dir || stopped || !context.isIdle()) return;
    ctx = context;
    const id = active;
    // Repeated settled events while paused must not republish the question as
    // an unowned manual completion or replace its immutable segment result.
    if (question && !id) return;
    const ordinary = assistantResult(messages);
    if (segmentAborted || ['aborted', 'error'].includes(ordinary.status)) question = undefined;
    const completion = question
      ? { status: 'waiting_question', question, text: '', messages }
      : { ...ordinary, ...(segmentAborted ? { status: 'aborted', text: '' } : {}), messages };
    active = undefined;
    syncTools();
    // Publish readiness before the completion file: immediate follow-ups must not see stale busy state.
    await status();
    await result(id, completion);
  });
  pi.on('session_shutdown', async () => {
    if (!dir) return;
    stopped = true;
    if (timer) cancel(timer);
    // Let an in-flight filesystem poll finish before the parent removes the directory.
    while (processing) await new Promise((resolve) => setTimeout(resolve, 10));
    if (active) await result(active, { status: 'error', text: '', errorMessage: 'Child session closed or replaced' });
    active = undefined;
    question = undefined;
    syncTools();
    await status();
  });
}
