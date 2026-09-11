import { randomUUID } from 'node:crypto';

/** Await pipe closure and cancellation escalation. processGroup requires a detached POSIX child. */
export function waitForProcess(proc, { signal, onAbort, processGroup = false, graceMs = 5000 } = {}) {
  return new Promise(resolve => {
    let exited = proc.exitCode != null || proc.signalCode != null;
    let settled = false, timer, closed = false, closeCode;
    let cancelling = false, escalationPending = false, groupGone = false;
    const grouped = processGroup && !!proc.pid;
    const send = name => {
      if (groupGone) return;
      try {
        // An exited launcher can leave descendants holding its output pipes open.
        if (grouped) process.kill(-proc.pid, name);
        else if (!exited) proc.kill(name);
      } catch (error) {
        // Never target this numeric group again after confirmed disappearance.
        if (grouped && error.code === 'ESRCH') groupGone = true;
        else if (name !== 0 && error.code !== 'ESRCH' && !exited) proc.kill(name);
      }
    };
    const endEscalation = () => {
      clearTimeout(timer);
      escalationPending = false;
      if (closed) finish(closeCode);
    };
    const abort = () => {
      if (settled || cancelling) return;
      cancelling = true;
      onAbort?.();
      escalationPending = grouped;
      const deadline = Date.now() + graceMs;
      const tick = () => {
        if (grouped) send(0);
        if (groupGone) return endEscalation();
        const remaining = deadline - Date.now();
        if (remaining > 0) { timer = setTimeout(tick, Math.min(50, remaining)); return; }
        send('SIGKILL');
        endEscalation();
      };
      // kill() may emit an error synchronously, so cleanup must already own the timer.
      timer = setTimeout(tick, Math.min(50, Math.max(0, graceMs)));
      send('SIGTERM');
      if (groupGone) endEscalation();
    };
    const exit = () => { exited = true; };
    const finish = code => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
      proc.removeListener('exit', exit);
      proc.removeListener('close', close);
      proc.removeListener('error', error);
      resolve(code);
    };
    const close = code => {
      closed = true;
      closeCode = code ?? 1;
      if (escalationPending) {
        // Pipe closure says nothing about descendants with redirected output.
        send(0);
        if (!groupGone) return;
        endEscalation();
      } else finish(closeCode);
    };
    const error = () => close(1);
    proc.on('exit', exit);
    proc.on('close', close);
    proc.on('error', error);
    signal?.addEventListener('abort', abort, { once: true });
    if (signal?.aborted) abort();
  });
}

const terminal = new Set(['completed', 'error', 'aborted']);
const occupied = new Set(['running', 'waiting_question']);

/** Session-owned execution segments. Waiting questions retain their capacity slot. */
export class Jobs {
  constructor({ capacity = 4, onChange, pollMs = 1000 } = {}) {
    if (!Number.isInteger(capacity) || capacity < 1) throw new Error('Invalid capacity');
    if (!Number.isFinite(pollMs) || pollMs < 1) throw new Error('Invalid pollMs');
    this.capacity = capacity;
    this.onChange = onChange;
    this.pollMs = pollMs;
    this.jobs = new Map();
    this.records = new Map();
    this.closed = false;
    this.pumping = false;
    this.timer = undefined;
  }

  start({ mode = 'single', tasks, run, resume, cancelMember, health }) {
    if (this.closed) throw new Error('Jobs registry is shut down');
    if (!['single', 'parallel', 'chain'].includes(mode)) throw new Error('Invalid job mode');
    if (!Array.isArray(tasks) || !tasks.length || typeof run !== 'function') throw new Error('Tasks and run are required');
    const id = randomUUID();
    const job = { id, mode, run, resume, cancelMember, health, revision: 0, listeners: new Set(),
      cancelled: false, previous: '', tasks: tasks.map((task, index) => ({
        task, index, agent: task.agent, state: 'queued', controller: new AbortController(), generation: 0,
        flights: new Set(), cleanups: new Map(), cleaning: 0,
      })) };
    this.jobs.set(id, job);
    this._change(job);
    this._pump();
    return id;
  }

  _job(id) {
    const job = this.jobs.get(id);
    if (!job) throw new Error(`Unknown job: ${id}`);
    return job;
  }

  snapshot(id) {
    const job = this._job(id);
    const states = job.tasks.map(task => task.state);
    const status = job.cancelled ? 'aborted'
      : states.includes('waiting_question') ? 'waiting_question'
      : states.some(state => state === 'queued' || state === 'running') ? 'running'
      : states.includes('error') ? 'error'
      : states.includes('aborted') ? 'aborted' : 'completed';
    const blockedByQuestions = states.every(state => state === 'queued') && this._slots() >= this.capacity
      ? [...this.records.values()].filter(record => record.live && record.job !== job).map(record => record.question.id) : [];
    return { jobId: id, revision: job.revision, mode: job.mode, status, blockedByQuestions,
      tasks: job.tasks.map(({ index, agent, state, result, cleanupError }) => ({ index, agent, state,
        ...(cleanupError ? { cleanupError } : {}),
        ...(result === undefined ? {} : { result: structuredClone(result) }) })) };
  }

  _change(job) {
    job.revision++;
    // A question in another job can explain why a queued dispatch cannot start.
    for (const owner of this.jobs.values()) for (const listener of [...owner.listeners]) listener();
    if (this.onChange) {
      try { Promise.resolve(this.onChange(this.snapshot(job.id))).catch(() => {}); } catch { /* Notification failures never own execution. */ }
    }
    this._pollSchedule();
  }

  _slots() {
    return [...this.jobs.values()].reduce((sum, job) => sum + job.tasks.filter(task =>
      occupied.has(task.state) || task.flights.size || task.cleaning || task.cleanupError).length, 0);
  }

  _pump() {
    if (this.pumping || this.closed) return;
    this.pumping = true;
    try {
      let count = this._slots();
      for (const job of this.jobs.values()) {
        if (job.cancelled) continue;
        for (const task of job.tasks) {
          if (count >= this.capacity) return;
          if (task.state !== 'queued') continue;
          if (job.mode === 'chain' && task.index > 0 && job.tasks[task.index - 1].state !== 'completed') continue;
          count++;
          this._execute(job, task, () => job.run(task.task, task.index, job.previous, task.controller.signal));
        }
      }
    } finally { this.pumping = false; }
  }

  _execute(job, task, worker) {
    task.state = 'running';
    const generation = ++task.generation;
    task.flights.add(generation);
    this._change(job);
    // The final catch also captures validation/settlement errors, not only worker errors.
    Promise.resolve().then(() => {
      if (task.controller.signal.aborted) return { status: 'aborted', text: 'Cancelled' };
      return worker();
    }).then(async result => {
      if (generation !== task.generation || job.cancelled) {
        if (result?.memberId) await this._cancelMember(job, task, result.memberId);
        return;
      }
      this._settle(job, task, result);
    }).catch(async error => {
      if (generation !== task.generation || job.cancelled) {
        if (error?.memberId) await this._cancelMember(job, task, error.memberId);
        return;
      }
      this._settle(job, task, { status: 'error', text: String(error?.message ?? error),
        memberId: error?.memberId ?? task.result?.memberId, cleanupError: error?.cleanupError });
    }).finally(() => { task.flights.delete(generation); this._pump(); });
  }

  _settle(job, task, result) {
    if (!result || !['completed', 'waiting_question', 'error', 'aborted'].includes(result.status)) throw new Error('Invalid worker outcome');
    // Clone before changing state so invalid non-data outcomes become ordinary errors.
    result = structuredClone(result);
    if (result.status === 'waiting_question') {
      const question = result.question;
      if (!question || typeof question.id !== 'string' || !question.id || typeof question.text !== 'string') throw new Error('Invalid question outcome');
      if (this.records.has(question.id)) throw new Error('Question ID already used');
      this.records.set(question.id, { job, task, question, live: true });
    }
    if (result.cleanupError && result.memberId) this._cancelMember(job, task, result.memberId);
    task.result = result;
    task.state = result.status;
    if (job.mode === 'chain') {
      if (result.status === 'completed') job.previous = result.text;
      if (result.status === 'error' || result.status === 'aborted') {
        for (const next of job.tasks.slice(task.index + 1)) {
          next.state = 'aborted';
          next.result = { status: 'aborted', text: 'Earlier chain step failed' };
          next.controller.abort();
        }
      }
    }
    this._change(job);
    this._pump();
  }

  question(id) {
    const record = this.records.get(id);
    if (!record?.live) return undefined;
    return { ...structuredClone(record.question), jobId: record.job.id, index: record.task.index,
      memberId: record.task.result?.memberId, question: structuredClone(record.question) };
  }

  questions() {
    return [...this.records.keys()].map(id => this.question(id)).filter(Boolean);
  }

  answer(id, text, { source = 'coordinator' } = {}) {
    if (this.closed) throw new Error('Unknown or stale question: registry is shut down');
    if (!['human', 'coordinator'].includes(source)) throw new Error('Invalid answer source');
    if (typeof text !== 'string' || !text.trim()) throw new Error('Answer text is required');
    const record = this.records.get(id);
    if (!record || record.job.cancelled || record.invalid) throw new Error('Unknown or stale question');
    if (record.question.requiresUser && source !== 'human') throw new Error('Question requires a human answer');
    if (record.accepted) {
      if (record.accepted.text !== text || record.accepted.source !== source) throw new Error('Question already answered differently');
      return { ...record.accepted.ack };
    }
    if (!record.live || record.task.state !== 'waiting_question') throw new Error('Stale question');
    const { job, task } = record;
    if (typeof job.resume !== 'function') throw new Error('Job does not support answers');
    const ack = { jobId: job.id, questionId: id, status: 'queued' };
    record.accepted = { text, source, ack };
    record.live = false;
    const outcome = task.result;
    this._execute(job, task, () => job.resume(outcome, text, source, task.controller.signal));
    return { ...ack };
  }

  wait(id, { signal, after, timeoutMs } = {}) {
    const job = this._job(id);
    return new Promise(resolve => {
      let timer;
      let settled = false;
      const finish = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        job.listeners.delete(check);
        signal?.removeEventListener('abort', abort);
        resolve(this.snapshot(id));
      };
      const check = () => {
        const snapshot = this.snapshot(id);
        if (terminal.has(snapshot.status) || (after === undefined
          ? snapshot.status === 'waiting_question' || snapshot.blockedByQuestions.length > 0 : snapshot.revision > after)) finish();
      };
      const abort = () => { this.cancel(id); finish(); };
      job.listeners.add(check);
      signal?.addEventListener('abort', abort, { once: true });
      if (signal?.aborted) abort();
      else check();
      if (!settled && timeoutMs !== undefined) timer = setTimeout(finish, Math.max(0, timeoutMs));
    });
  }

  _cancelMember(job, task, memberId = task.result?.memberId) {
    if (!memberId || !job.cancelMember) return Promise.resolve();
    if (task.cleanups.has(memberId)) return task.cleanups.get(memberId);
    if (!task.result?.memberId) task.result = { ...task.result, memberId };
    task.cleaning++;
    let cleanup;
    try { cleanup = Promise.resolve(job.cancelMember(memberId)); } catch (error) { cleanup = Promise.reject(error); }
    cleanup = cleanup.catch(error => {
      // Do not pretend capacity is free after failed PTY cleanup.
      task.cleanupError = `Pane cleanup failed: ${error?.message ?? error}`;
      this._change(job);
    }).finally(() => { task.cleaning--; this._pump(); });
    task.cleanups.set(memberId, cleanup);
    return cleanup;
  }

  /** Called only after the manager confirms a member was closed successfully. */
  memberClosed(memberId) {
    for (const job of this.jobs.values()) {
      for (const task of job.tasks) {
        if (task.result?.memberId !== memberId && !task.cleanups.has(memberId)) continue;
        delete task.cleanupError;
        task.cleanups.delete(memberId);
        if (occupied.has(task.state)) {
          task.generation++;
          task.controller.abort();
          for (const record of this.records.values()) {
            if (record.task === task) { record.live = false; record.invalid = true; }
          }
          this._settle(job, task, { ...task.result, question: undefined, cleanupError: undefined,
            status: 'aborted', text: 'Team member closed' });
        } else this._change(job);
      }
    }
    this._pump();
  }

  cancel(id) {
    const job = this._job(id);
    if (job.cancelled || terminal.has(this.snapshot(id).status)) return this.snapshot(id);
    job.cancelled = true;
    for (const record of this.records.values()) {
      if (record.job === job) { record.live = false; record.invalid = true; }
    }
    for (const task of job.tasks) {
      if (terminal.has(task.state)) continue;
      task.generation++;
      task.controller.abort();
      this._cancelMember(job, task);
      task.state = 'aborted';
      task.result = { ...task.result, status: 'aborted', text: 'Cancelled' };
      delete task.result.question;
    }
    this._change(job);
    this._pump();
    return this.snapshot(id);
  }

  _pollSchedule() {
    const needed = !this.closed && [...this.records.values()].some(record => record.live && record.job.health);
    if (!needed && this.timer) { clearTimeout(this.timer); this.timer = undefined; }
    if (!needed || this.timer) return;
    this.timer = setTimeout(() => {
      this.timer = undefined;
      for (const record of this.records.values()) {
        if (!record.live || !record.job.health || record.checking) continue;
        record.checking = true;
        Promise.resolve().then(() => record.job.health(record.task.result?.memberId)).then(health => {
          if (health === false || health == null || health.alive === false || health.status === 'gone' || health.status === 'error' || health.status === 'aborted') {
            throw new Error('Waiting member is unavailable');
          }
        }).catch(error => {
          if (!record.live) return;
          record.live = false;
          record.invalid = true;
          record.task.controller.abort();
          this._cancelMember(record.job, record.task);
          this._settle(record.job, record.task, { status: 'error', text: String(error?.message ?? error), memberId: record.task.result?.memberId });
        }).finally(() => { record.checking = false; });
      }
      this._pollSchedule();
    }, this.pollMs);
    this.timer.unref?.();
  }

  shutdown() {
    this.closed = true;
    clearTimeout(this.timer);
    this.timer = undefined;
    for (const job of this.jobs.values()) this.cancel(job.id);
  }
}
