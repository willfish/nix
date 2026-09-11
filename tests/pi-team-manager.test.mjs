import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readdir, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';
import { TeamManager, CHILD_EXTENSION, launchCommand, shellQuote, teamAvailable } from '../home/config/pi/extensions/subagent/team.js';
import { atomicJson, envelope, readJson } from '../home/config/pi/extensions/subagent/protocol.js';

async function until(predicate) {
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
    const value = await predicate();
    if (value) return value;
    await delay(2);
  }
  assert.fail('Timed out waiting for fake child IPC');
}

// A fake pane transport, not a fake protocol: all child state, commands and
// results cross the real private directory using the production JSON envelope.
class FakeHerdrPanes {
  owned = new Set();
  opened = [];
  closed = [];
  calls = [];
  children = new Map();
  seen = [];
  queuedAtClose = [];
  failures = [];
  ready = true;
  autoReply = true;
  missing = false;
  initialState = {};
  async open(cwd, env, label) {
    if (this.openError) throw this.openError;
    const paneId = `child-${this.opened.length + 1}`;
    this.opened.push({ paneId, cwd, env, label });
    this.owned.add(paneId);
    return paneId;
  }
  async call(method, params) {
    this.calls.push({ method, params });
    assert.equal(method, 'pane.send_input');
    if (this.launchError) throw this.launchError;
    const record = [...this.manager.records.values()].find((entry) => entry.paneId === params.pane_id);
    assert.ok(record);
    const child = { record, running: true };
    this.children.set(record.paneId, child);
    await this.status(record, { idle: true, stopped: false, ...this.initialState });
    if (this.ready) await this.write(record, 'ready.json', { pid: 1234 });
    child.loop = this.poll(child).catch((error) => this.failures.push(error));
  }
  write(record, file, value) { return atomicJson(join(record.dir, file), envelope(record.runId, value)); }
  status(record, extra = {}) {
    return this.write(record, 'status.json', {
      idle: false, stopped: false, updated: Date.now(),
      sessionId: `session-${record.id}`, sessionFile: `/session/${record.id}.jsonl`, ...extra,
    });
  }
  async respond(record, data, extra = {}) {
    const result = { commandId: data.id, status: 'completed', text: `answer: ${data.text}`,
      sessionId: `session-${record.id}`, ...extra };
    await this.status(record, { idle: true });
    await this.write(record, 'last.json', result);
    await this.write(record, `results/${data.id}.json`, result);
  }
  async poll(child) {
    const { record } = child;
    while (child.running) {
      for (const name of await readdir(join(record.dir, 'commands'))) {
        if (!name.endsWith('.json')) continue;
        const data = await readJson(join(record.dir, 'commands', name), record.runId);
        await rm(join(record.dir, 'commands', name));
        if (data.kind === 'prompt') {
          await this.status(record);
          if (this.autoReply) await this.respond(record, data);
        } else if (data.kind === 'retire') {
          const status = await this.onRetire?.(record, data) ?? 'retired';
          await this.write(record, `results/${data.id}.json`, { commandId: data.id, status });
        } else {
          await this.write(record, `results/${data.id}.json`, { commandId: data.id, status: 'accepted' });
        }
        this.seen.push({ record, data });
      }
      await delay(2);
    }
  }
  async exists(id) { return !this.missing && this.owned.has(id); }
  async close(id) {
    assert.notEqual(id, 'parent');
    assert.notEqual(id, 'unrelated');
    if (!this.owned.has(id)) return;
    if (this.closeError) throw this.closeError;
    await this.beforeClose?.(id);
    const child = this.children.get(id);
    if (child) {
      child.running = false;
      await child.loop;
      for (const name of await readdir(join(child.record.dir, 'commands'))) {
        if (name.endsWith('.json')) this.queuedAtClose.push(await readJson(join(child.record.dir, 'commands', name), child.record.runId));
      }
    }
    this.closed.push(id);
    this.owned.delete(id);
  }
}

async function fixture(t, options = {}) {
  const root = await mkdtemp(join(tmpdir(), 'pi-manager-test-'));
  const panes = new FakeHerdrPanes();
  const manager = new TeamManager({ panes, root, startupMs: 2000, taskMs: 2000, pollMs: 2, ...options });
  panes.manager = manager;
  const invocations = [];
  const invocation = (flags) => {
    invocations.push(flags);
    return { command: '/path with spaces/pi', args: ['--model', "model's name", ...flags] };
  };
  t.after(async () => {
    panes.closeError = undefined;
    await manager.shutdown();
    await rm(root, { recursive: true, force: true });
    assert.deepEqual(panes.failures, [], 'fake child polling must not fail');
  });
  return { root, panes, manager, invocations,
    run: (extra = {}) => manager.run({ agent: 'builder', task: 'initial task', cwd: "/work/it's a project", invocation, ...extra }),
    prompt: () => until(() => panes.seen.find(({ data }) => data.kind === 'prompt')),
  };
}

test('launch command safely quotes every argument and clears only stale Pi metadata', () => {
  const args = ['space value', "quote's", '$(touch /never-run-this)', '; echo unsafe', '', 'line\nbreak'];
  const launch = launchCommand({ command: process.execPath, args: ['-e', 'console.log(JSON.stringify({args:process.argv.slice(1),env:process.env}))', '--', ...args] });
  const stale = { PI_SESSION_ID: 'old', PI_SESSION_FILE: 'old', PI_PROVIDER: 'old', PI_MODEL: 'old', PI_REASONING_LEVEL: 'old' };
  const output = JSON.parse(execFileSync('/bin/sh', ['-c', launch], { encoding: 'utf8', env: {
    ...process.env, ...stale, HERDR_PANE_ID: 'new-child', HERDR_SOCKET_PATH: '/socket', KEEP_ME: 'yes',
  } }));
  assert.deepEqual(output.args, args);
  for (const key of Object.keys(stale)) assert.equal(output.env[key], undefined);
  assert.equal(output.env.PI_TEAM_CHILD, '1');
  assert.equal(output.env.HERDR_PANE_ID, 'new-child');
  assert.equal(output.env.HERDR_SOCKET_PATH, '/socket');
  assert.equal(output.env.KEEP_ME, 'yes');
  assert.throws(() => shellQuote('bad\0arg'), /Invalid shell argument/);
  assert.throws(() => shellQuote(123), /Invalid shell argument/);
});

test('availability requires interactive Herdr parent and refuses recursive children', () => {
  const env = { HERDR_ENV: '1', HERDR_SOCKET_PATH: '/socket', HERDR_PANE_ID: 'parent' };
  assert.equal(teamAvailable({ mode: 'tui' }, env), true);
  assert.equal(teamAvailable({ mode: 'rpc' }, env), false);
  assert.equal(teamAvailable({ mode: 'tui' }, { ...env, PI_TEAM_CHILD: '1' }), false);
  for (const key of Object.keys(env)) assert.equal(teamAvailable({ mode: 'tui' }, { ...env, [key]: '' }), false);
});

test('startup creates private IPC, launches interactive child, retains result and sends in the same session', async (t) => {
  const f = await fixture(t);
  const first = await f.run();
  const record = f.manager.get(first.memberId);
  assert.equal((await stat(record.dir)).mode & 0o777, 0o700);
  assert.equal((await readJson(join(record.dir, 'request.json'), record.runId)).agent, 'builder');
  assert.deepEqual(f.invocations, [['--extension', CHILD_EXTENSION, '--team-run', record.dir]]);
  assert.deepEqual(f.panes.opened[0], { paneId: first.paneId, cwd: "/work/it's a project", env: { PI_TEAM_CHILD: '1' }, label: `pi: builder [${first.memberId}]` });
  assert.deepEqual(f.panes.calls[0].params.keys, ['Enter']);
  assert.equal(f.panes.calls[0].params.text, launchCommand({ command: '/path with spaces/pi', args: ['--model', "model's name", ...f.invocations[0]] }));
  assert.equal(first.status, 'completed');
  assert.equal(first.text, 'answer: initial task');
  assert.equal(record.pending, false);
  assert.equal((await f.manager.read(record.id)).commandId, first.commandId);
  const second = await f.manager.send(record.id, 'continue that work');
  assert.equal(second.memberId, first.memberId);
  assert.equal(second.paneId, first.paneId);
  assert.equal(second.sessionId, first.sessionId);
  assert.notEqual(second.commandId, first.commandId);
  assert.equal(f.panes.opened.length, 1);
  assert.equal(f.panes.closed.length, 0);
  const [listed] = await f.manager.list();
  assert.equal(listed.id, record.id);
  assert.equal(listed.pending, false);
  assert.equal(listed.idle, true);
  assert.equal((await f.manager.read(record.id)).text, 'answer: continue that work');
});

test('pending tasks reject send, accept steer, report progress and retain completed context', async (t) => {
  const f = await fixture(t);
  f.panes.autoReply = false;
  const progress = [];
  const running = f.run({ onProgress: (messages) => progress.push(messages) });
  const { record, data } = await f.prompt();
  assert.deepEqual(await f.manager.read(record.id), { status: 'running', text: '' });
  await assert.rejects(f.manager.send(record.id, 'conflict'), /pending task; use steer/);
  const steer = await f.manager.steer(record.id, 'new guidance');
  assert.equal(steer.status, 'accepted');
  await until(() => f.panes.seen.some(({ data }) => data.kind === 'steer' && data.text === 'new guidance'));
  const messages = [{ role: 'assistant', content: [{ type: 'text', text: 'working' }] }];
  await f.panes.write(record, 'progress.json', { updated: Date.now(), messages });
  await until(() => progress.length);
  assert.deepEqual(progress, [messages]);
  await f.panes.respond(record, data);
  assert.equal((await running).memberId, record.id);
  assert.equal(record.pending, false);
});

test('manual busy sessions reject send and accept steer without acquiring a task', async (t) => {
  const f = await fixture(t);
  const result = await f.run();
  const record = f.manager.get(result.memberId);
  await f.panes.status(record, { idle: false });
  await assert.rejects(f.manager.send(record.id, 'no'), /busy; use steer/);
  assert.equal(record.pending, false);
  assert.equal((await f.manager.steer(record.id, 'manual guidance')).status, 'accepted');
  assert.equal(f.panes.seen.filter(({ data }) => data.kind === 'prompt').length, 1);
});

test('default capacity four evicts the oldest idle member, never a manual-busy member', async (t) => {
  const f = await fixture(t);
  const results = [];
  for (let i = 0; i < 4; i++) results.push(await f.run({ task: `task ${i}` }));
  const first = f.manager.get(results[0].memberId);
  const second = f.manager.get(results[1].memberId);
  await f.panes.status(first, { idle: false });
  const fifth = await f.run();
  assert.equal(f.manager.records.size, 4);
  assert.deepEqual(f.panes.closed, [second.paneId]);
  assert.equal(f.manager.get(first.id), first);
  assert.ok(f.manager.get(fifth.memberId));
  await assert.rejects(stat(second.dir), { code: 'ENOENT' });
  for (const record of f.manager.records.values()) await f.panes.status(record, { idle: false });
  await assert.rejects(f.run(), /All four team panes are busy/);
  assert.equal(f.panes.opened.length, 5);
  assert.deepEqual(f.panes.closed, [second.paneId]);
});

test('pending members cannot be evicted even if their last heartbeat says idle', async (t) => {
  const f = await fixture(t, { capacity: 1 });
  f.panes.autoReply = false;
  const running = f.run();
  const { record, data } = await f.prompt();
  await f.panes.status(record, { idle: true });
  await assert.rejects(f.run(), /busy/);
  assert.deepEqual(f.panes.closed, []);
  await f.panes.respond(record, data);
  await running;
});

for (const phase of ['startup', 'task', 'send']) {
  test(`cancellation during ${phase} sends abort and removes owned pane and IPC`, async (t) => {
    const f = await fixture(t);
    const controller = new AbortController();
    let running, record;
    if (phase === 'send') {
      const first = await f.run();
      record = f.manager.get(first.memberId);
      f.panes.autoReply = false;
      running = f.manager.send(record.id, 'follow up', { signal: controller.signal });
      await until(() => f.panes.seen.filter(({ data }) => data.kind === 'prompt').length === 2);
    } else {
      f.panes.ready = phase !== 'startup';
      f.panes.autoReply = false;
      running = f.run({ signal: controller.signal });
      if (phase === 'task') ({ record } = await f.prompt());
      else record = await until(() => [...f.manager.records.values()][0]);
    }
    const rejected = assert.rejects(running, /was aborted/);
    controller.abort();
    await rejected;
    assert.equal(f.manager.records.size, 0);
    assert.deepEqual(f.panes.closed, [record.paneId]);
    // Abort delivery is best effort: it may still be queued when the PTY closes.
    assert.ok(f.panes.seen.some(({ data }) => data.kind === 'abort') ||
      f.panes.queuedAtClose.some((data) => data?.kind === 'abort'));
    await assert.rejects(stat(record.dir), { code: 'ENOENT' });
  });
}

test('follow-up timeout aborts and closes only its owned pane and removes IPC', async (t) => {
  const f = await fixture(t);
  const retained = await f.run();
  const first = await f.run();
  const record = f.manager.get(first.memberId);
  f.panes.autoReply = false;
  await assert.rejects(f.manager.send(record.id, 'follow up', { timeoutMs: 60 }), /Team task timed out/);
  assert.equal(record.pending, false);
  assert.equal(record.closed, true);
  assert.deepEqual(f.panes.closed, [record.paneId]);
  assert.equal(f.manager.records.size, 1);
  assert.ok(f.manager.get(retained.memberId));
  assert.ok(f.panes.seen.some(({ record: entry, data }) => entry === record && data.kind === 'abort') ||
    f.panes.queuedAtClose.some((data) => data?.kind === 'abort'));
  await assert.rejects(stat(record.dir), { code: 'ENOENT' });
});

for (const acknowledgement of ['retired', 'busy']) {
  test(`retirement reservation rejects concurrent send and steer while awaiting ${acknowledgement}`, async (t) => {
    const f = await fixture(t, { capacity: 1 });
    const first = await f.run();
    const record = f.manager.get(first.memberId);
    let release, requested = false;
    const gate = new Promise((resolve) => { release = resolve; });
    f.panes.onRetire = async () => { requested = true; await gate; return acknowledgement; };
    const replacement = f.run();
    const outcome = acknowledgement === 'busy'
      ? assert.rejects(replacement, /All four team panes are busy/)
      : replacement;
    try {
      await until(() => requested);
      assert.equal(record.retiring, true);
      await assert.rejects(f.manager.send(record.id, 'racing prompt'), /being retired/);
      await assert.rejects(f.manager.steer(record.id, 'racing guidance'), /being retired/);
      assert.deepEqual(f.panes.closed, []);
      assert.equal(f.panes.opened.length, 1);
      assert.equal(record.pending, false);
      assert.equal(record.steering ?? 0, 0);
      assert.equal((await readdir(join(record.dir, 'commands'))).length, 0);
    } finally { release(); }
    await outcome;
    assert.equal(record.retiring, false);
    assert.ok(!f.panes.seen.some(({ data }) => data.text?.startsWith('racing')));
    if (acknowledgement === 'busy') {
      assert.deepEqual(f.panes.closed, []);
      assert.equal(f.manager.get(record.id), record);
      assert.equal((await f.manager.send(record.id, 'still usable')).status, 'completed');
    } else {
      assert.deepEqual(f.panes.closed, [record.paneId]);
      assert.equal(f.panes.opened.length, 2);
      await assert.rejects(stat(record.dir), { code: 'ENOENT' });
    }
  });
}

test('shutdown racing follow-up cancellation preserves the session-closed error and closes once', async (t) => {
  const f = await fixture(t);
  const first = await f.run();
  const record = f.manager.get(first.memberId);
  f.panes.autoReply = false;
  const running = f.manager.send(record.id, 'follow up');
  const rejected = assert.rejects(running, { message: 'Team session closed' });
  await until(() => f.panes.seen.filter(({ data }) => data.kind === 'prompt').length === 2);
  // Hold the serialized shutdown close until send has entered cancellation.
  // Its queued cleanup must recheck ownership instead of masking the wait error.
  f.panes.beforeClose = () => until(() => f.panes.seen.some(({ data }) => data.kind === 'abort'));
  await f.manager.shutdown();
  await rejected;
  assert.equal(record.pending, false);
  assert.deepEqual(f.panes.closed, [record.paneId]);
  assert.equal(f.manager.records.size, 0);
  assert.equal(f.panes.owned.size, 0);
  await assert.rejects(stat(record.dir), { code: 'ENOENT' });
});

test('pre-aborted run does not allocate a pane or IPC directory', async (t) => {
  const f = await fixture(t);
  await assert.rejects(f.run({ signal: AbortSignal.abort() }), /was aborted/);
  assert.deepEqual(f.panes.opened, []);
  assert.deepEqual(await readdir(f.root), []);
});

for (const phase of ['startup', 'task']) {
  test(`${phase} timeout cleans up the child and its directory`, async (t) => {
    const f = await fixture(t, { startupMs: phase === 'startup' ? 60 : 2000, taskMs: 60 });
    f.panes.ready = phase !== 'startup';
    f.panes.autoReply = false;
    await assert.rejects(f.run(), new RegExp(`Team ${phase} timed out`));
    assert.equal(f.manager.records.size, 0);
    assert.equal(f.panes.closed.length, 1);
    assert.deepEqual(await readdir(f.root), []);
  });
}

for (const [name, setup, error] of [
  ['missing pane', (f) => { f.panes.missing = true; }, /Agent pane was closed/],
  ['stopped child', (f) => { f.panes.initialState = { stopped: true }; }, /Agent session stopped/],
  ['stale heartbeat', (f) => { f.panes.initialState = { updated: Date.now() - 20000 }; }, /heartbeat stopped/],
]) {
  test(`${name} fails promptly and cleans up during startup`, async (t) => {
    const f = await fixture(t);
    f.panes.ready = false;
    setup(f);
    await assert.rejects(f.run(), error);
    assert.equal(f.manager.records.size, 0);
    assert.deepEqual(await readdir(f.root), []);
  });
}

for (const stage of ['open', 'launch']) {
  test(`${stage} failure does not leak IPC or owned panes`, async (t) => {
    const f = await fixture(t);
    f.panes[`${stage}Error`] = new Error(`${stage} failed`);
    await assert.rejects(f.run(), new RegExp(`${stage} failed`));
    assert.equal(f.manager.records.size, 0);
    assert.equal(f.panes.owned.size, 0);
    assert.deepEqual(await readdir(f.root), []);
  });
}

test('mismatched task result identity is rejected rather than returned', async (t) => {
  const f = await fixture(t);
  f.panes.autoReply = false;
  const running = f.run();
  const rejected = assert.rejects(running, /Team result command mismatch/);
  const { record, data } = await f.prompt();
  await f.panes.respond(record, data, { commandId: 'wrong-command' });
  await rejected;
  assert.equal(f.manager.records.size, 0);
});

test('shutdown closes retained and orphaned owned panes, not parent or unrelated panes', async (t) => {
  const f = await fixture(t);
  await f.run();
  await f.run();
  f.panes.owned.add('orphaned-owned');
  await f.manager.shutdown();
  assert.deepEqual(f.panes.closed, ['child-1', 'child-2', 'orphaned-owned']);
  assert.equal(f.manager.records.size, 0);
  assert.equal(f.panes.owned.size, 0);
  assert.deepEqual(await readdir(f.root), []);
  await assert.rejects(f.run(), /manager has stopped/);
  await f.manager.shutdown();
  assert.equal(f.panes.closed.length, 3);
  assert.throws(() => f.manager.get('unknown'), /Unknown team member/);
});

test('shutdown interrupts an active wait and removes its pane', async (t) => {
  const f = await fixture(t);
  f.panes.autoReply = false;
  const running = f.run();
  const rejected = assert.rejects(running, /Team session closed/);
  await f.prompt();
  // Keep shutdown in progress until the waiting task requests cancellation.
  // Assert externally visible behaviour, not which public method cleanup uses.
  f.panes.beforeClose = () => until(() => f.panes.seen.some(({ data }) => data.kind === 'abort'));
  await f.manager.shutdown();
  assert.equal(f.manager.records.size, 0);
  assert.equal(f.panes.closed.length, 1);
  await rejected;
});

test('shutdown reports close failures and preserves ownership for a retry', async (t) => {
  const f = await fixture(t);
  const result = await f.run();
  f.panes.closeError = new Error('transport unavailable');
  await assert.rejects(f.manager.shutdown(), (error) => {
    assert.ok(error instanceof AggregateError);
    assert.ok(error.errors.every((item) => item.message === 'transport unavailable'));
    return true;
  });
  assert.ok(f.panes.owned.has(result.paneId));
  assert.ok(f.manager.get(result.memberId));
  f.panes.closeError = undefined;
  await f.manager.shutdown();
  assert.equal(f.panes.owned.size, 0);
});

test('already-cancelled steering never publishes guidance or closes an idle member', async (t) => {
  const f = await fixture(t);
  const member = await f.run();
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(f.manager.steer(member.memberId, 'must not run', { signal: controller.signal }), /aborted/);
  assert.equal(f.panes.seen.filter(({ data }) => data.kind === 'steer').length, 0);
  assert.deepEqual(await readdir(join(f.manager.get(member.memberId).dir, 'commands')), []);
  assert.equal(f.manager.records.size, 1);
});

test('unacknowledged steering times out and closes the owned member', async (t) => {
  const f = await fixture(t, { startupMs: 100 });
  const member = await f.run();
  const originalWrite = f.panes.write.bind(f.panes);
  f.panes.write = async (record, file, data) => {
    if (file.startsWith('results/') && data.status === 'accepted') return;
    return originalWrite(record, file, data);
  };
  await assert.rejects(f.manager.steer(member.memberId, 'ambiguous delivery'), /timed out/);
  assert.equal(f.manager.records.size, 0);
  assert.deepEqual(f.panes.closed, [member.paneId]);
});
