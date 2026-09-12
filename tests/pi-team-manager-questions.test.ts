import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readdir, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { TeamManager } from '../home/config/pi/extensions/subagent/team.ts';
import { Jobs } from '../home/config/pi/extensions/subagent/jobs.ts';
import { prepareRun, atomicJson, envelope, readJson } from '../home/config/pi/extensions/subagent/protocol.ts';

async function until(fn) {
  for (let i = 0; i < 1000; i++) { const value = await fn(); if (value) return value; await delay(2); }
  assert.fail('Timed out waiting for manager IPC');
}

// Seed a retained, settled member at the pane boundary. Commands and outcomes
// still use production IPC, with explicit settlement instead of a polling child.
async function fixture(t) {
  const dir = await mkdtemp(join(tmpdir(), 'pi-manager-questions-'));
  const record = { id: 'member', runId: 'run', paneId: 'owned', dir, agent: 'builder', pending: false, closed: false };
  const closed = [], atClose = [];
  const panes = { owned: new Set(['owned']), exists: async id => panes.owned.has(id),
    async close(id) { atClose.push(...await commands()); closed.push(id); panes.owned.delete(id); } };
  const manager = new TeamManager({ panes, capacity: 1, pollMs: 2, taskMs: 2000 });
  manager.records.set(record.id, record);
  await prepareRun(dir, { runId: record.runId });
  const question = { id: 'question-1', text: 'Which target?', commandId: 'segment-1', requiresUser: false };
  const write = (file, data) => atomicJson(join(dir, file), envelope(record.runId, data));
  const status = (extra = {}) => write('status.json', { idle: false, updated: Date.now(), question, ...extra });
  async function commands() {
    const files = await readdir(join(dir, 'commands'));
    return Promise.all(files.filter(name => name.endsWith('.json')).map(name => readJson(join(dir, 'commands', name), record.runId)));
  }
  await status();
  t.after(async () => { await manager.shutdown(); await rm(dir, { recursive: true, force: true }); });
  return { manager, record, panes, closed, atClose, question, write, status, commands,
    answer: (text = 'target A', options) => manager.answer(record.id, question.id, text, options),
    next: () => until(async () => (await commands()).find(c => c.kind === 'answer')) };
}

for (const mode of ['running', 'waiting', 'failed-cleanup', 'failed-running-throw', 'failed-running-outcome']) test(`real manager scheduling retains full capacity during ${mode} cancellation`, async t => {
  const root = await mkdtemp(join(tmpdir(), 'pi-manager-capacity-'));
  let releaseClose;
  const closeGate = new Promise(resolve => { releaseClose = resolve; });
  const opened = [], closing = [], closed = [], dispatched = [];
  let heldPane, failClose = mode.startsWith('failed-');
  let manager;
  const write = (record, file, data) => atomicJson(join(record.dir, file), envelope(record.runId, data));
  const panes = {
    owned: new Set(),
    async open() {
      const id = `pane-${opened.length}`;
      opened.push(id); this.owned.add(id); return id;
    },
    async exists(id) { return this.owned.has(id); },
    async call(method, params) {
      assert.equal(method, 'pane.send_input');
      const record = [...manager.records.values()].find(record => record.paneId === params.pane_id);
      assert.ok(record);
      await write(record, 'status.json', { idle: false, updated: Date.now() });
      await write(record, 'ready.json', { pid: 1234 });
    },
    async close(id) {
      closing.push(id);
      if (id === heldPane) {
        await closeGate;
        if (failClose) throw new Error('deliberate pane close failure');
      }
      closed.push(id); this.owned.delete(id);
    },
  };
  manager = new TeamManager({ panes, root, capacity: 4, pollMs: 2, startupMs: 2000, taskMs: 10000 });
  const jobs = new Jobs({ capacity: 4 });
  t.after(async () => {
    failClose = false; releaseClose(); jobs.shutdown();
    await manager.shutdown();
    await rm(root, { recursive: true, force: true });
  });
  const start = agent => jobs.start({ tasks: [{ agent }],
    run: async (task, index, previous, signal) => {
      dispatched.push(agent);
      try {
        return await manager.run({ ...task, task: 'work', cwd: root, signal,
          invocation: args => ({ command: 'fake-pi', args }) });
      } catch (error) {
        if (!mode.startsWith('failed-running') || !error.cleanupError) throw error;
        assert.equal(error.memberId, records[0].id);
        assert.equal(error.cleanupError, 'deliberate pane close failure');
        if (mode === 'failed-running-throw') throw error;
        // Match index's error-outcome adapter without dropping cleanup ownership.
        return { status: signal.aborted ? 'aborted' : 'error', text: error.message,
          memberId: error.memberId, cleanupError: error.cleanupError };
      }
    },
    cancelMember: id => manager.close(id),
  });
  const prompt = record => until(async () => {
    const names = await readdir(join(record.dir, 'commands'));
    for (const name of names.filter(name => name.endsWith('.json'))) {
      const data = await readJson(join(record.dir, 'commands', name), record.runId);
      if (data?.kind === 'prompt') return data;
    }
  });
  const ids = Array.from({ length: 4 }, (_, index) => start(`worker-${index}`));
  await until(() => manager.records.size === 4);
  const records = [...manager.records.values()];
  const prompts = await Promise.all(records.map(prompt));
  heldPane = records[0].paneId;
  // Other members stay busy in real manager IPC, so an early dispatch would fail all-four-busy.
  if (mode === 'waiting' || mode === 'failed-cleanup') {
    const question = { id: 'held-question', commandId: prompts[0].id, text: 'Continue?' };
    await write(records[0], 'status.json', { idle: false, updated: Date.now(), question });
    await write(records[0], `results/${prompts[0].id}.json`, {
      commandId: prompts[0].id, status: 'waiting_question', question, text: '',
    });
    assert.equal((await jobs.wait(ids[0], { timeoutMs: 1000 })).status, 'waiting_question');
  }
  const queued = start('replacement');
  assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
  if (mode.startsWith('failed-running')) assert.equal(jobs.snapshot(ids[0]).tasks[0].result, undefined);
  jobs.cancel(ids[0]);
  await until(() => closing.includes(heldPane));
  await delay(30);
  assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
  assert.equal(dispatched.length, 4, 'queued work must not reach TeamManager during pane cleanup');
  assert.equal(opened.length, 4);
  assert.equal(panes.owned.size, 4);
  assert.equal(manager.records.has(records[0].id), true);
  releaseClose();
  if (mode.startsWith('failed-')) {
    await until(() => jobs.snapshot(ids[0]).tasks[0].cleanupError);
    assert.match(jobs.snapshot(ids[0]).tasks[0].cleanupError, /deliberate pane close failure/);
    await delay(30);
    assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
    assert.equal(dispatched.length, 4);
    assert.equal(panes.owned.size, 4);
    assert.equal(manager.records.has(records[0].id), true);
    assert.equal(jobs.snapshot(ids[0]).tasks[0].result.memberId, records[0].id);
    failClose = false;
    await manager.close(records[0].id);
    jobs.memberClosed(records[0].id);
    assert.equal(jobs.snapshot(ids[0]).tasks[0].cleanupError, undefined);
  }
  {
    const replacement = await until(() => [...manager.records.values()].find(record => record.agent === 'replacement'));
    assert.ok(closed.includes(heldPane));
    assert.equal(manager.records.has(records[0].id), false);
    assert.equal(opened.length, 5);
    const command = await prompt(replacement);
    await write(replacement, 'status.json', { idle: true, updated: Date.now() });
    await write(replacement, `results/${command.id}.json`, { commandId: command.id, status: 'completed', text: 'replacement succeeded' });
    const result = await jobs.wait(queued, { timeoutMs: 1000 });
    assert.equal(result.status, 'completed');
    assert.equal(result.tasks[0].result.text, 'replacement succeeded');
  }
});

test('manual close of a waiting real member reconciles its job and is repeatable', async t => {
  const f = await fixture(t);
  const jobs = new Jobs({ capacity: 1 });
  t.after(() => jobs.shutdown());
  const id = jobs.start({ mode: 'chain', tasks: [{ agent: 'builder' }, { agent: 'next' }],
    run: async () => ({ status: 'waiting_question', memberId: f.record.id, question: f.question, text: '' }) });
  await jobs.wait(id);
  const queued = jobs.start({ tasks: [{ agent: 'replacement' }], run: async () => ({ status: 'completed', text: 'new work' }) });
  assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
  await f.manager.close(f.record.id);
  jobs.memberClosed(f.record.id);
  await f.manager.close(f.record.id);
  jobs.memberClosed(f.record.id);
  assert.deepEqual(f.closed, ['owned']);
  assert.deepEqual(jobs.snapshot(id).tasks.map(task => task.state), ['aborted', 'aborted']);
  assert.equal(jobs.questions().length, 0);
  assert.throws(() => jobs.answer(f.question.id, 'late'), /stale/);
  assert.equal((await jobs.wait(queued, { timeoutMs: 1000 })).status, 'completed');
});

for (const adapter of ['throw', 'outcome']) test(`resumed timeout preserves failed-close ownership via ${adapter} until manual retry`, async t => {
  const f = await fixture(t);
  const jobs = new Jobs({ capacity: 1 });
  const close = f.panes.close;
  let attempts = 0;
  f.panes.close = async () => { attempts++; throw new Error('resume close failed'); };
  t.after(() => { f.panes.close = close; jobs.shutdown(); });
  const id = jobs.start({ tasks: [{ agent: 'builder' }],
    run: async () => ({ status: 'waiting_question', memberId: f.record.id, question: f.question, text: '' }),
    resume: async (outcome, text, source, signal) => {
      try { return await f.manager.answer(outcome.memberId, outcome.question.id, text, { source, signal, timeoutMs: 80 }); }
      catch (error) {
        assert.equal(error.memberId, f.record.id);
        assert.equal(error.cleanupError, 'resume close failed');
        if (adapter === 'throw') throw error;
        return { status: 'error', text: error.message, memberId: error.memberId, cleanupError: error.cleanupError };
      }
    }, cancelMember: id => f.manager.close(id) });
  await jobs.wait(id);
  jobs.answer(f.question.id, 'yes');
  await f.next();
  const queued = jobs.start({ tasks: [{ agent: 'replacement' }], run: async () => ({ status: 'completed', text: 'recovered' }) });
  await until(() => jobs.snapshot(id).tasks[0].cleanupError);
  const failed = jobs.snapshot(id);
  assert.equal(failed.status, 'error');
  assert.equal(failed.tasks[0].result.memberId, f.record.id);
  assert.match(failed.tasks[0].cleanupError, /resume close failed/);
  assert.ok(attempts >= 2, 'Jobs retries the failed internal manager cleanup');
  assert.equal(f.manager.records.has(f.record.id), true);
  await delay(30);
  assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
  f.panes.close = close;
  await f.manager.close(f.record.id);
  jobs.memberClosed(f.record.id);
  assert.equal(jobs.snapshot(id).tasks[0].cleanupError, undefined);
  assert.equal((await jobs.wait(queued, { timeoutMs: 1000 })).status, 'completed');
});

test('waiting questions expose identity and health, retain capacity, and reject send/steer without destruction', async t => {
  const f = await fixture(t);
  assert.deepEqual(await f.manager.questions(), [{ ...f.question, memberId: f.record.id }]);
  assert.equal(await f.manager.health(f.record.id), true);
  await assert.rejects(f.manager.send(f.record.id, 'restart'), /busy/);
  await assert.rejects(f.manager.steer(f.record.id, 'guidance'), /question pending/);
  // Even a stale idle flag must not make a question evictable.
  await f.status({ idle: true });
  await assert.rejects(f.manager.run({ agent: 'other', task: 'replacement' }), /busy/);
  assert.equal(f.record.pending, false);
  assert.equal(f.record.steering, 0);
  assert.deepEqual(await f.commands(), []);
  assert.deepEqual(f.closed, []);
  assert.equal(f.manager.get(f.record.id), f.record);
});

test('health rejects missing, closed, stopped, stale, and no-question members', async t => {
  const f = await fixture(t);
  assert.equal(await f.manager.health('unknown'), false);
  for (const extra of [{ stopped: true }, { updated: Date.now() - 20000 }, { question: undefined }]) {
    await f.status(extra);
    assert.equal(await f.manager.health(f.record.id), false);
  }
  await f.status();
  f.record.closed = true;
  assert.equal(await f.manager.health(f.record.id), false);
  f.record.closed = false;
  f.panes.owned.delete(f.record.paneId);
  assert.equal(await f.manager.health(f.record.id), false);
  f.panes.owned.add(f.record.paneId);
  await rm(join(f.record.dir, 'status.json'));
  assert.equal(await f.manager.health(f.record.id), false);
});

test('invalid, stale, unsettled and unauthorized answers do not publish commands or destroy a question', async t => {
  const f = await fixture(t);
  for (const [text, options, error] of [[' ', {}, /text is required/], ['yes', { source: 'tool' }, /Invalid answer source/], ['yes', { signal: AbortSignal.abort() }, /aborted/]]) {
    await assert.rejects(f.answer(text, options), error);
  }
  await assert.rejects(f.manager.answer(f.record.id, 'old-question', 'yes'), /Stale/);
  await f.status({ commandId: 'segment-1' });
  await assert.rejects(f.answer(), /unsettled/);
  await f.status({ question: undefined });
  await assert.rejects(f.answer(), /Stale/);
  f.question.requiresUser = true;
  await f.status();
  await assert.rejects(f.answer(), /human answer/);
  f.record.pending = true;
  await assert.rejects(f.answer('yes', { source: 'human' }), /pending execution segment/);
  assert.equal(f.record.pending, true, 'another segment retains its reservation');
  f.record.pending = false;
  assert.deepEqual(await f.commands(), []);
  assert.deepEqual(await f.manager.questions(), [{ ...f.question, memberId: f.record.id }]);
  assert.deepEqual(f.closed, []);
});

for (const source of ['coordinator', 'human']) test(`${source} answer binds question and prior segment and awaits a distinct next segment`, async t => {
  const f = await fixture(t);
  f.question.requiresUser = source === 'human';
  await f.status();
  let settled = false;
  const answer = f.answer('chosen target', { source }).then(value => { settled = true; return value; });
  const command = await f.next();
  assert.equal(command.questionId, f.question.id);
  assert.equal(command.questionCommandId, f.question.commandId);
  assert.equal(command.source, source);
  assert.equal(command.text, 'chosen target');
  assert.notEqual(command.id, f.question.commandId);
  assert.equal(f.record.pending, true);
  assert.equal(settled, false);
  await assert.rejects(f.answer('duplicate', { source }), /pending execution segment/);
  const nextQuestion = { ...f.question, id: 'question-2', commandId: command.id };
  await f.status({ question: nextQuestion });
  const outcome = { commandId: command.id, status: 'waiting_question', question: nextQuestion, text: '' };
  await f.write('last.json', outcome);
  await f.write(`results/${command.id}.json`, outcome);
  const result = await answer;
  assert.equal(result.status, 'waiting_question');
  assert.equal(result.commandId, command.id);
  assert.equal(result.memberId, f.record.id);
  assert.equal(result.paneId, f.record.paneId);
  assert.equal(f.record.pending, false);
  await assert.rejects(f.answer('late', { source }), /Stale/);
  assert.deepEqual(await f.manager.questions(), [{ ...nextQuestion, memberId: f.record.id }]);
  assert.deepEqual(f.closed, []);
});

for (const reason of ['cancel', 'timeout', 'mismatch']) test(`answer ${reason} cleans owned pane and IPC after dispatch`, async t => {
  const f = await fixture(t);
  const controller = new AbortController();
  const answer = f.answer('yes', { signal: controller.signal, timeoutMs: reason === 'timeout' ? 80 : 2000 });
  const rejected = assert.rejects(answer, reason === 'cancel' ? /aborted/ : reason === 'timeout' ? /timed out/ : /command mismatch/);
  const command = await f.next();
  if (reason === 'cancel') controller.abort();
  if (reason === 'mismatch') await f.write(`results/${command.id}.json`, { commandId: 'wrong', status: 'completed' });
  await rejected;
  assert.equal(f.record.pending, false);
  assert.equal(f.manager.records.size, 0);
  assert.deepEqual(f.closed, ['owned']);
  assert.ok(f.atClose.some(c => c.kind === 'abort'));
  await assert.rejects(stat(f.record.dir), { code: 'ENOENT' });
});
