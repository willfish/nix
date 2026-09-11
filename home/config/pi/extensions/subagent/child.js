import { readFile, readdir, rename, rm, stat } from 'node:fs/promises';
import { join, isAbsolute } from 'node:path';
import { atomicJson, readJson, envelope, assistantResult, VERSION } from './protocol.js';
import { socketCall } from './herdr.js';

/** Explicitly loaded only for interactive team children. No terminal scraping. */
export default function childExtension(pi, options = {}) {
  pi.registerFlag('team-run', { description: 'Private coordinator IPC directory', type: 'string' });
  let dir, request, ctx, timer, processing = false, stopped = false, active;
  let messages = [];
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
    pid: process.pid, idle: ctx.isIdle() && !active, commandId: active,
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
          if (data.kind === 'prompt') {
            if (active || !ctx.isIdle()) {
              await result(data.id, { status: 'error', text: '', errorMessage: 'Agent is busy; use steer or wait' });
              continue;
            }
            if (typeof data.text !== 'string' || !data.text.trim()) throw new Error('Empty team prompt');
            active = data.id;
            messages = [];
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
    // Never let a child create an uncontrolled recursive team, even via headless delegation.
    pi.setActiveTools(pi.getActiveTools().filter((name) => !['subagent', 'team'].includes(name)));
    timer = every(() => { void poll(); }, 200);
    timer.unref?.();
    await status();
    await write('ready.json', { pid: process.pid });
  });
  pi.on('agent_start', async () => {
    if (!dir || stopped) return;
    if (!active) messages = [];
    await status();
  });
  pi.on('message_end', async (event) => {
    if (!dir || stopped || event.message?.role !== 'assistant') return;
    messages.push(event.message);
    // Tool output remains in the real Pi session. Bound coordinator IPC independently.
    while (Buffer.byteLength(JSON.stringify(messages)) > 6 * 1024 * 1024 && messages.length > 1) messages.shift();
    await write('progress.json', { messages, updated: Date.now() });
  });
  pi.on('agent_settled', async (_event, context) => {
    if (!dir || stopped || !context.isIdle()) return;
    ctx = context;
    const id = active;
    const completion = { ...assistantResult(messages), messages };
    active = undefined;
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
    await status();
  });
}
