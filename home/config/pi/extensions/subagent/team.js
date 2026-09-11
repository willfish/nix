import { mkdtemp, rm, chmod } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { HerdrPanes } from './herdr.js';
import { prepareRun, command, readJson, sleep, atomicJson, envelope } from './protocol.js';

export const CHILD_EXTENSION = fileURLToPath(new URL('./child.js', import.meta.url));
export function shellQuote(value) {
  if (typeof value !== 'string' || value.includes('\0')) throw new Error('Invalid shell argument');
  return `'${value.replaceAll("'", "'\\''")}'`;
}
export function launchCommand(invocation) {
  // Herdr supplies NEW pane identity. Strip only stale Pi session metadata.
  return ['exec', 'env', '-u', 'PI_SESSION_ID', '-u', 'PI_SESSION_FILE', '-u', 'PI_PROVIDER',
    '-u', 'PI_MODEL', '-u', 'PI_REASONING_LEVEL', 'PI_TEAM_CHILD=1',
    invocation.command, ...invocation.args].map(shellQuote).join(' ');
}
export function teamAvailable(ctx, env = process.env) {
  return ctx.mode === 'tui' && env.HERDR_ENV === '1' && !!env.HERDR_SOCKET_PATH &&
    !!env.HERDR_PANE_ID && env.PI_TEAM_CHILD !== '1';
}

export class TeamManager {
  records = new Map();
  #queue = Promise.resolve();
  stopped = false;
  constructor({ env = process.env, panes, startupMs = 60000, taskMs = 30 * 60 * 1000,
    pollMs = 200, capacity = 4, root = tmpdir() } = {}) {
    this.panes = panes ?? new HerdrPanes({ socketPath: env.HERDR_SOCKET_PATH, parentPaneId: env.HERDR_PANE_ID });
    Object.assign(this, { startupMs, taskMs, pollMs, capacity, root });
    this.leaseTimer = setInterval(() => {
      for (const record of this.records.values()) {
        if (!record.closed) void atomicJson(join(record.dir, 'lease.json'),
          envelope(record.runId, { updated: Date.now() })).catch(() => {});
      }
    }, 1000);
    this.leaseTimer.unref();
  }
  #serialize(fn) {
    const result = this.#queue.then(fn);
    this.#queue = result.catch(() => {});
    return result;
  }
  async state(record) { return readJson(join(record.dir, 'status.json'), record.runId); }
  async list() {
    return Promise.all([...this.records.values()].map(async (record) => ({
      id: record.id, agent: record.agent, paneId: record.paneId, cwd: record.cwd,
      pending: record.pending, ...await this.state(record),
      ...(record.cleanupError ? { status: 'error', cleanupError: record.cleanupError } : {}),
    })));
  }
  get(id) {
    const record = this.records.get(id);
    if (!record || record.closed) throw new Error(`Unknown team member: ${id}`);
    if (record.retiring) throw new Error('Agent is being retired; dispatch a new member');
    return record;
  }
  async read(id) {
    const record = this.get(id);
    if (record.cleanupError) return { status: 'error', memberId: record.id, paneId: record.paneId,
      cleanupError: record.cleanupError, text: 'Pane cleanup pending; close this member' };
    return await readJson(join(record.dir, 'last.json'), record.runId) ?? { status: 'running', text: '' };
  }
  async questions() {
    const members = await this.list();
    return members.filter(member => member.question).map(member => ({ ...member.question, memberId: member.id }));
  }
  async health(id) {
    const record = this.records.get(id);
    if (!record || record.closed || !await this.panes.exists(record.paneId)) return false;
    const state = await this.state(record);
    return !!state && !state.stopped && Date.now() - state.updated <= 15000 && !!state.question;
  }
  async #close(record) {
    // Closing an owned pane terminates its whole PTY, not just the launcher process.
    try { await this.panes.close(record.paneId); }
    catch (cause) {
      record.cleanupError = String(cause?.message ?? cause);
      throw Object.assign(new Error(`Pane cleanup failed for ${record.id}: ${cause.message}`, { cause }), {
        memberId: record.id, cleanupError: cause.message,
      });
    }
    record.closed = true;
    this.records.delete(record.id);
    await rm(record.dir, { recursive: true, force: true });
  }
  close(id) {
    return this.#serialize(() => {
      const record = this.records.get(id);
      return record ? this.#close(record) : undefined;
    });
  }
  async shutdown() {
    this.stopped = true;
    clearInterval(this.leaseTimer);
    return this.#serialize(async () => {
      const errors = [];
      for (const record of [...this.records.values()]) {
        try { await this.#close(record); } catch (error) { errors.push(error); }
      }
      // A split may have succeeded while its setup/cleanup failed before a record existed.
      for (const paneId of [...this.panes.owned]) {
        if ([...this.records.values()].some(record => record.paneId === paneId)) continue;
        try { await this.panes.close(paneId); } catch (error) { errors.push(error); }
      }
      if (errors.length) throw new AggregateError(errors, 'Some team panes could not be closed');
    });
  }
  async #wait(record, file, { signal, timeoutMs = this.taskMs, onProgress } = {}) {
    const deadline = Date.now() + timeoutMs;
    let lastCheck = 0, lastProgress = 0;
    while (Date.now() < deadline) {
      if (signal?.aborted) throw new Error('Subagent was aborted');
      if (record.closed || this.stopped) throw new Error('Team session closed');
      const data = await readJson(join(record.dir, file), record.runId);
      if (data) return data;
      if (Date.now() - lastCheck > 2000) {
        lastCheck = Date.now();
        if (!await this.panes.exists(record.paneId)) throw new Error('Agent pane was closed');
        const state = await this.state(record);
        if (state?.stopped) throw new Error('Agent session stopped');
        if (state && Date.now() - state.updated > 15000) throw new Error('Agent bridge heartbeat stopped');
      }
      if (onProgress) {
        const progress = await readJson(join(record.dir, 'progress.json'), record.runId);
        if (progress && progress.updated > lastProgress) {
          lastProgress = progress.updated;
          onProgress(progress.messages);
        }
      }
      await sleep(this.pollMs);
    }
    throw new Error(`Team ${file === 'ready.json' ? 'startup' : 'task'} timed out`);
  }
  async #cancelAndClose(record) {
    if (!record.closed && this.records.has(record.id)) {
      // Best effort graceful abort before the bounded hard stop.
      try { await command(record.dir, record.runId, 'abort'); await sleep(Math.min(300, this.pollMs * 2)); } catch {}
      await this.#serialize(async () => {
        if (!record.closed && this.records.has(record.id)) await this.#close(record);
      });
    }
  }
  async run({ agent, task, cwd, invocation, signal, onProgress }) {
    if (signal?.aborted) throw new Error('Subagent was aborted');
    const record = await this.#serialize(async () => {
      if (this.stopped) throw new Error('Team manager has stopped');
      if (this.records.size >= this.capacity) {
        let retired = false;
        for (const candidate of this.records.values()) {
          const state = await this.state(candidate);
          if (candidate.cleanupError || candidate.pending || candidate.steering || candidate.retiring || state?.question || (!state?.idle && !state?.stopped)) continue;
          candidate.retiring = true; // Reserve before any await; send/steer cannot race retirement.
          try {
            if (!state?.stopped) {
              const id = await command(candidate.dir, candidate.runId, 'retire');
              const ack = await this.#wait(candidate, `results/${id}.json`, { timeoutMs: this.startupMs });
              if (ack.status !== 'retired') continue;
            }
            await this.#close(candidate);
            retired = true;
            break;
          } finally { candidate.retiring = false; }
        }
        if (!retired) throw new Error('All four team panes are busy; wait or close a member');
      }
      const dir = await mkdtemp(join(this.root, 'pi-team-'));
      await chmod(dir, 0o700);
      const runId = randomUUID();
      const entry = { id: runId.slice(0, 8), runId, dir, agent, cwd, pending: true, closed: false };
      try {
        await prepareRun(dir, { runId, agent });
        entry.paneId = await this.panes.open(cwd, { PI_TEAM_CHILD: '1', PI_TEAM_ROLE: agent }, `pi: ${agent} [${entry.id}]`);
        this.records.set(entry.id, entry);
        await atomicJson(join(dir, 'request.json'), envelope(runId, {
          parentPid: process.pid, paneId: entry.paneId, runId, agent,
        }));
        const launch = invocation(['--extension', CHILD_EXTENSION, '--team-run', dir]);
        await this.panes.call('pane.send_input', { pane_id: entry.paneId,
          text: launchCommand(launch), keys: ['Enter'] });
        return entry;
      } catch (error) {
        // open() can allocate successfully before both setup and rollback fail.
        if (!entry.paneId && error?.paneId && this.panes.owned.has(error.paneId)) {
          entry.paneId = error.paneId;
          entry.cleanupError = error.cleanupError;
          this.records.set(entry.id, entry);
        }
        entry.pending = false;
        if (entry.paneId) await this.#close(entry);
        else await rm(dir, { recursive: true, force: true });
        throw error;
      }
    });
    try {
      await this.#wait(record, 'ready.json', { signal, timeoutMs: this.startupMs });
      return await this.#ask(record, task, { signal, onProgress });
    } catch (error) {
      await this.#cancelAndClose(record);
      throw error;
    } finally { record.pending = false; }
  }
  async #ask(record, text, options) {
    const id = await command(record.dir, record.runId, 'prompt', text);
    const result = await this.#wait(record, `results/${id}.json`, options);
    if (result.commandId !== id) throw new Error('Team result command mismatch');
    return { ...result, memberId: record.id, paneId: record.paneId };
  }
  async send(id, text, options = {}) {
    const record = this.get(id);
    if (record.cleanupError) throw new Error('Pane cleanup pending; close this member');
    if (record.pending) throw new Error('Agent has a pending task; use steer');
    record.pending = true;
    let dispatched = false;
    try {
      const state = await this.state(record);
      if (!state?.idle) throw new Error('Agent is busy; use steer');
      dispatched = true;
      return await this.#ask(record, text, options);
    } catch (error) {
      if (dispatched) await this.#cancelAndClose(record);
      throw error;
    } finally { record.pending = false; }
  }
  async answer(id, questionId, text, { source = 'coordinator', ...options } = {}) {
    if (options.signal?.aborted) throw new Error('Subagent was aborted');
    if (!['coordinator', 'human'].includes(source)) throw new Error('Invalid answer source');
    if (typeof text !== 'string' || !text.trim()) throw new Error('Answer text is required');
    const record = this.get(id);
    if (record.cleanupError) throw new Error('Pane cleanup pending; close this member');
    if (record.pending) throw new Error('Agent already has a pending execution segment');
    record.pending = true;
    let dispatched = false;
    try {
      const state = await this.state(record);
      const question = state?.question;
      if (!question || question.id !== questionId || state.commandId) throw new Error('Stale or unsettled question');
      if (question.requiresUser && source !== 'human') throw new Error('Question requires a human answer');
      const commandId = await command(record.dir, record.runId, 'answer', text, {
        questionId, questionCommandId: question.commandId, source,
      });
      dispatched = true;
      const result = await this.#wait(record, `results/${commandId}.json`, options);
      if (result.commandId !== commandId) throw new Error('Team result command mismatch');
      return { ...result, memberId: record.id, paneId: record.paneId };
    } catch (error) {
      if (dispatched) await this.#cancelAndClose(record);
      throw error;
    } finally { record.pending = false; }
  }
  async steer(id, text, options = {}) {
    if (options.signal?.aborted) throw new Error('Subagent was aborted');
    const record = this.get(id);
    if (record.cleanupError) throw new Error('Pane cleanup pending; close this member');
    record.steering = (record.steering ?? 0) + 1;
    let dispatched = false;
    try {
      if ((await this.state(record))?.question) throw new Error('Coordinator question pending; answer or cancel it first');
      const commandId = await command(record.dir, record.runId, 'steer', text);
      dispatched = true;
      const result = await this.#wait(record, `results/${commandId}.json`, { ...options, timeoutMs: this.startupMs });
      return { ...result, memberId: record.id };
    } catch (error) {
      // Delivery is ambiguous after publication. Stop the child rather than leave failed guidance queued.
      if (dispatched) await this.#cancelAndClose(record);
      throw error;
    } finally { record.steering--; }
  }
}
