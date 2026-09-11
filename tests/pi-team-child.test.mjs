import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, chmod, readdir, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomUUID } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import childExtension from '../home/config/pi/extensions/subagent/child.js';
import { atomicJson, command, envelope, prepareRun, readJson } from '../home/config/pi/extensions/subagent/protocol.js';

async function until(predicate) {
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline) {
    const value = await predicate();
    if (value) return value;
    await delay(2);
  }
  assert.fail('Timed out waiting for child IPC');
}

async function fixture(t, options = {}) {
  const dir = await mkdtemp(join(tmpdir(), 'pi-child-test-'));
  await chmod(dir, 0o700);
  const runId = randomUUID();
  await prepareRun(dir, { runId, agent: 'builder' });
  const hooks = new Map(), registered = new Map(), sent = [], notices = [], flags = [];
  let pendingMessages = false;
  let tools = ['read', 'bash', 'subagent', 'team', 'custom'], idle = true, aborts = 0, shutdowns = 0;
  let timer, cancelled = 0;
  const ctx = {
    mode: options.mode ?? 'tui', isIdle: () => idle, hasPendingMessages: () => pendingMessages,
    sessionManager: { getSessionId: () => 'private-session', getSessionFile: () => '/sessions/child.jsonl' },
    ui: { notify: (...args) => notices.push(args) },
    abort: async () => { aborts++; },
    shutdown: () => { shutdowns++; },
  };
  const pi = {
    registerTool: (tool) => registered.set(tool.name, tool),
    registerFlag: (...args) => flags.push(args), getFlag: () => Object.hasOwn(options, 'flag') ? options.flag : dir,
    on: (name, hook) => hooks.set(name, hook),
    getActiveTools: () => tools, setActiveTools: (value) => { tools = value; },
    sendUserMessage: (...args) => sent.push(args),
  };
  childExtension(pi, {
    ...options.bridge,
    every: (fn) => (timer = setInterval(fn, 2)),
    cancel: (handle) => { cancelled++; clearInterval(handle); },
  });
  const emit = (name, event = {}, context = ctx) => hooks.get(name)?.(event, context);
  t.after(async () => {
    await emit('session_shutdown');
    clearInterval(timer);
    await rm(dir, { recursive: true, force: true });
  });
  return {
    dir, runId, ctx, emit, sent, notices, flags, registered,
    pending: (value) => { pendingMessages = value; },
    get tools() { return tools; }, get aborts() { return aborts; }, get cancelled() { return cancelled; }, get shutdowns() { return shutdowns; },
    idle: (value) => { idle = value; },
    start: () => emit('session_start'),
    read: (file) => readJson(join(dir, file), runId),
    send: (kind, text, extra) => command(dir, runId, kind, text, extra),
    result: (id) => until(() => readJson(join(dir, `results/${id}.json`), runId)),
  };
}
const toolMessage = (names = ['ask_coordinator']) => ({
  role: 'assistant', stopReason: 'toolUse',
  content: names.map((name, index) => ({ type: 'toolCall', name, id: `tool-${index}`, arguments: { text: 'Which option?' } })),
});
async function ask(f, params = { text: 'Which option?' }) {
  await f.emit('message_end', { message: toolMessage() });
  assert.equal(await f.emit('tool_call', { toolName: 'ask_coordinator', toolCallId: 'tool-0', input: params }), undefined);
  return f.registered.get('ask_coordinator').execute('tool-0', params, undefined, undefined, f.ctx);
}
const answerExtra = (question, source = 'coordinator') => ({ questionId: question.id, questionCommandId: question.commandId, source });

const assistant = (text, stopReason = 'stop', extra = {}) => ({
  role: 'assistant', content: [{ type: 'text', text }], stopReason, ...extra,
});

test('child registers its flag, publishes private ready/session status, and disables recursive tools', async (t) => {
  const f = await fixture(t);
  await f.start();
  assert.deepEqual(f.flags, [['team-run', { description: 'Private coordinator IPC directory', type: 'string' }]]);
  assert.deepEqual(f.tools, ['read', 'bash', 'custom']);
  assert.equal((await stat(f.dir)).mode & 0o777, 0o700);
  assert.equal((await stat(join(f.dir, 'ready.json'))).mode & 0o777, 0o600);
  assert.equal((await f.read('ready.json')).pid, process.pid);
  const state = await f.read('status.json');
  assert.equal(state.runId, f.runId);
  assert.equal(state.idle, true);
  assert.equal(state.stopped, false);
  assert.equal(state.sessionId, 'private-session');
  assert.equal(state.sessionFile, '/sessions/child.jsonl');
  await f.start(); // Repeated session_start must not replace the bridge.
  assert.equal(f.cancelled, 0);
});

for (const [name, options, setup, error] of [
  ['noninteractive mode', { mode: 'rpc' }, null, /requires interactive Pi/],
  ['relative flag', { flag: 'relative/path' }, null, /Invalid team IPC directory/],
  ['non-string flag', { flag: true }, null, /Invalid team IPC directory/],
  ['public directory', {}, (f) => chmod(f.dir, 0o755), /must be private/],
  ['wrong protocol version', {}, (f) => atomicJson(join(f.dir, 'request.json'), { version: 99, runId: f.runId }), /Invalid team request/],
  ['missing run ID', {}, (f) => atomicJson(join(f.dir, 'request.json'), { version: 1 }), /Invalid team request/],
]) {
  test(`child rejects ${name} before becoming ready`, async (t) => {
    const f = await fixture(t, options);
    await setup?.(f);
    await assert.rejects(f.start(), error);
    assert.equal(await f.read('ready.json'), null);
    assert.ok(f.tools.includes('team'));
  });
}

test('without the flag the extension does nothing, including lifecycle events', async (t) => {
  const f = await fixture(t, { flag: undefined });
  await f.start();
  await f.emit('message_end', { message: assistant('ignored') });
  await f.emit('agent_settled');
  assert.equal(await f.read('ready.json'), null);
  assert.deepEqual(f.sent, []);
  assert.ok(f.tools.includes('subagent'));
});

test('prompt and busy steer use explicit delivery modes; results appear only when fully settled', async (t) => {
  const f = await fixture(t);
  await f.start();
  const id = await f.send('prompt', 'Implement {{literal}}');
  await until(() => f.sent.length === 1);
  assert.deepEqual(f.sent[0], ['Implement {{literal}}', { deliverAs: 'followUp', expandPromptTemplates: false }]);
  f.idle(false);
  await f.emit('agent_start');
  await f.emit('message_end', { message: { role: 'toolResult', content: 'not assistant output' } });
  await f.emit('message_end', { message: assistant('intermediate', 'toolUse') });
  assert.equal((await f.read('progress.json')).messages.length, 1);
  await f.emit('agent_end');
  await f.emit('agent_settled');
  assert.equal(await f.read(`results/${id}.json`), null);
  assert.equal(await f.read('last.json'), null);
  const guidance = await f.send('steer', 'Use the existing API');
  assert.equal((await f.result(guidance)).status, 'accepted');
  assert.deepEqual(f.sent[1], ['Use the existing API', { deliverAs: 'steer', expandPromptTemplates: false }]);
  const busy = await f.send('prompt', 'Do not start another task');
  assert.match((await f.result(busy)).errorMessage, /busy; use steer/);
  await f.emit('message_end', { message: assistant('final answer') });
  f.idle(true);
  await f.emit('agent_settled');
  const result = await f.result(id);
  assert.equal(result.status, 'completed');
  assert.equal(result.text, 'final answer');
  assert.equal(result.messages.length, 2);
  assert.equal(result.sessionId, 'private-session');
  assert.equal((await f.read('status.json')).idle, true);
  assert.equal((await f.read('last.json')).commandId, id);
  // Later polls must not replay claimed prompt or steer commands.
  for (let n = 0; n < 2; n++) await f.result(await f.send('abort'));
  assert.equal(f.sent.length, 2);
  await until(async () => !(await readdir(f.dir)).some((name) => name.startsWith('claimed-')));
  assert.deepEqual(await readdir(join(f.dir, 'commands')), []);
});

test('manual turns replace last result only at agent_settled and reset previous messages', async (t) => {
  const f = await fixture(t);
  await f.start();
  const id = await f.send('prompt', 'coordinated');
  await until(() => f.sent.length === 1);
  await f.emit('message_end', { message: assistant('coordinated answer') });
  await f.emit('agent_settled');
  await f.emit('agent_start');
  f.idle(false);
  await f.emit('message_end', { message: assistant('manual answer') });
  await f.emit('agent_end');
  assert.equal((await f.read('last.json')).commandId, id);
  f.idle(true);
  await f.emit('agent_settled');
  const last = await f.read('last.json');
  assert.equal(last.text, 'manual answer');
  assert.equal(last.commandId, undefined);
  assert.deepEqual(last.messages, [assistant('manual answer')]);
  assert.deepEqual(await readdir(join(f.dir, 'results')), [`${id}.json`]);
});

for (const [reason, text, expected] of [['error', '', 'error'], ['aborted', 'partial', 'aborted'], ['length', 'partial', 'incomplete'], ['stop', '', 'incomplete'], ['toolUse', 'I will run tests', 'incomplete']]) {
  test(`settled ${reason}/${JSON.stringify(text)} produces ${expected}`, async (t) => {
    const f = await fixture(t);
    await f.start();
    const id = await f.send('prompt', 'task');
    await until(() => f.sent.length === 1);
    await f.emit('message_end', { message: assistant(text, reason, { errorMessage: 'provider detail' }) });
    assert.equal(await f.read(`results/${id}.json`), null);
    await f.emit('agent_settled');
    const result = await f.result(id);
    assert.equal(result.status, expected);
    assert.equal(result.stopReason, reason);
    assert.equal(result.errorMessage, 'provider detail');
  });
}

test('abort is acknowledged separately and the active result waits for settlement', async (t) => {
  const f = await fixture(t);
  await f.start();
  const id = await f.send('prompt', 'long task');
  await until(() => f.sent.length === 1);
  const abortId = await f.send('abort');
  assert.equal((await f.result(abortId)).status, 'accepted');
  assert.equal(f.aborts, 1);
  assert.equal(await f.read(`results/${id}.json`), null);
  await f.emit('message_end', { message: assistant('', 'aborted') });
  await f.emit('agent_settled');
  assert.equal((await f.result(id)).status, 'aborted');
});

for (const [kind, text, error] of [['prompt', ' ', /Empty team prompt/], ['steer', '', /Empty team guidance/], ['unknown', 'x', /Unknown team command/]]) {
  test(`invalid ${kind} command stops bridge without delivery`, async (t) => {
    const f = await fixture(t);
    await f.start();
    await f.send(kind, text);
    await until(() => f.notices.length);
    assert.match(f.notices[0][0], error);
    assert.equal((await f.read('status.json')).stopped, true);
    assert.equal(f.cancelled, 1);
    assert.deepEqual(f.sent, []);
  });
}

test('a manual-busy child rejects coordinator prompts without taking over the turn', async (t) => {
  const f = await fixture(t);
  await f.start();
  f.idle(false);
  await f.emit('agent_start');
  const id = await f.send('prompt', 'interrupt manual work');
  const result = await f.result(id);
  assert.equal(result.status, 'error');
  assert.match(result.errorMessage, /busy; use steer/);
  assert.deepEqual(f.sent, []);
  await f.emit('message_end', { message: assistant('manual work finished') });
  f.idle(true);
  await f.emit('agent_settled');
  assert.equal((await f.read('last.json')).commandId, undefined);
  assert.equal((await f.read('last.json')).text, 'manual work finished');
});

test('foreign-run commands are rejected before delivery', async (t) => {
  const f = await fixture(t);
  await f.start();
  await command(f.dir, randomUUID(), 'prompt', 'wrong coordinator');
  await until(() => f.notices.length);
  assert.match(f.notices[0][0], /IPC identity mismatch/);
  assert.deepEqual(f.sent, []);
  assert.equal((await f.read('status.json')).stopped, true);
});

test('settlement without assistant output is incomplete, never a fabricated success', async (t) => {
  const f = await fixture(t);
  await f.start();
  const id = await f.send('prompt', 'task');
  await until(() => f.sent.length === 1);
  await f.emit('agent_settled');
  const result = await f.result(id);
  assert.equal(result.status, 'incomplete');
  assert.equal(result.text, '');
  assert.deepEqual(result.messages, []);
});

test('malformed command identity fails the active request and stops polling', async (t) => {
  const f = await fixture(t);
  await f.start();
  const active = await f.send('prompt', 'task');
  await until(() => f.sent.length === 1);
  const id = randomUUID();
  await atomicJson(join(f.dir, 'commands', `${id}.json`), envelope(f.runId, { id: randomUUID(), kind: 'steer', text: 'x', created: Date.now() }));
  assert.match((await f.result(active)).errorMessage, /command ID mismatch/);
  await until(() => f.notices.length);
  assert.equal((await f.read('status.json')).stopped, true);
});

test('idle retirement shuts down before acknowledging and stops heartbeat updates', async (t) => {
  const f = await fixture(t);
  await f.start();
  let shutdownRequested = false;
  f.ctx.shutdown = () => { shutdownRequested = true; };
  const id = await f.send('retire');
  const result = await f.result(id);
  assert.equal(shutdownRequested, true);
  assert.equal(result.status, 'retired');
  assert.equal(result.commandId, id);
  const state = await until(async () => {
    const state = await f.read('status.json');
    return state.stopped && state;
  });
  assert.equal(state.idle, true);
  assert.equal(f.cancelled, 1);
  await delay(30);
  assert.deepEqual(await f.read('status.json'), state);
  await f.send('prompt', 'too late');
  await delay(20);
  assert.deepEqual(f.sent, []);
});

for (const busy of ['manual turn', 'active command with idle context']) {
  test(`retirement declines a ${busy} without stopping the bridge`, async (t) => {
    const f = await fixture(t);
    await f.start();
    let active;
    if (busy === 'manual turn') f.idle(false);
    else {
      active = await f.send('prompt', 'queued task');
      await until(() => f.sent.length === 1);
      assert.equal(f.ctx.isIdle(), true);
    }
    const id = await f.send('retire');
    const result = await f.result(id);
    assert.equal(result.status, 'busy');
    assert.equal(result.commandId, id);
    assert.equal(f.shutdowns, 0);
    assert.equal(f.cancelled, 0);
    assert.equal((await f.read('status.json')).stopped, false);
    if (active) assert.equal(await f.read(`results/${active}.json`), null);
    f.idle(true);
    await f.emit('message_end', { message: assistant('finished') });
    await f.emit('agent_settled');
    assert.equal((await f.read('last.json')).text, 'finished');
    assert.equal((await f.result(await f.send('retire'))).status, 'retired');
    assert.equal(f.shutdowns, 1);
  });
}

for (const lease of ['expired', 'missing']) {
  test(`${lease} coordinator lease aborts the active task, shuts down, and stops the heartbeat`, async (t) => {
    const f = await fixture(t, { bridge: { leaseMs: 10000, terminate: async () => {} } });
    await f.start();
    const id = await f.send('prompt', 'orphaned task');
    await until(() => f.sent.length === 1);
    f.idle(false);
    if (lease === 'missing') await rm(join(f.dir, 'lease.json'));
    else await atomicJson(join(f.dir, 'lease.json'), envelope(f.runId, { updated: Date.now() - 20000 }));
    const result = await f.result(id);
    assert.equal(result.commandId, id);
    assert.equal(result.status, 'aborted');
    assert.equal(result.text, '');
    assert.equal(result.errorMessage, 'Coordinator lease expired');
    await until(() => f.shutdowns === 1);
    assert.equal(f.aborts, 1);
    assert.equal(f.cancelled, 1);
    const state = await f.read('status.json');
    assert.equal(state.stopped, true);
    assert.equal(state.commandId, undefined);
    await f.emit('message_end', { message: assistant('late success') });
    f.idle(true);
    await f.emit('agent_settled');
    await delay(30);
    assert.deepEqual(await f.read('status.json'), state);
    assert.deepEqual(await f.read('last.json'), result);
    assert.deepEqual(await f.result(id), result);
    assert.equal(f.aborts, 1);
    assert.equal(f.shutdowns, 1);
  });
}

test('shutdown fails an active request, stops the timer, and ignores later messages', async (t) => {
  const f = await fixture(t);
  await f.start();
  const id = await f.send('prompt', 'task');
  await until(() => f.sent.length === 1);
  await f.emit('session_shutdown');
  const result = await f.result(id);
  assert.equal(result.status, 'error');
  assert.match(result.errorMessage, /closed or replaced/);
  assert.equal((await f.read('status.json')).stopped, true);
  assert.equal(f.cancelled, 1);
  await f.emit('message_end', { message: assistant('too late') });
  await f.emit('agent_settled');
  assert.deepEqual(await f.read('last.json'), result);
});

test('delayed busy writes cannot overtake settlement; last result is visible before completion', async (t) => {
  let blockBusy = false;
  const busyStarted = Promise.withResolvers(), releaseBusy = Promise.withResolvers();
  const lastStarted = Promise.withResolvers(), releaseLast = Promise.withResolvers();
  t.after(() => { releaseBusy.resolve(); releaseLast.resolve(); });
  const f = await fixture(t, { bridge: { writeJson: async (file, value) => {
    if (file.endsWith('/status.json') && blockBusy && !value.idle) {
      blockBusy = false;
      busyStarted.resolve();
      await releaseBusy.promise;
    }
    if (file.endsWith('/last.json')) { lastStarted.resolve(); await releaseLast.promise; }
    await atomicJson(file, value);
  } } });
  await f.start();
  blockBusy = true;
  const id = await f.send('prompt', 'task');
  await busyStarted.promise;
  // Settlement is queued while the old busy status is still being written.
  const message = f.emit('message_end', { message: assistant('final') });
  const settled = f.emit('agent_settled');
  await delay(20);
  assert.equal(await f.read(`results/${id}.json`), null);
  releaseBusy.resolve();
  await lastStarted.promise;
  assert.equal((await f.read('status.json')).idle, true);
  assert.equal(await f.read(`results/${id}.json`), null, 'Completion must wait for latest-result publication');
  releaseLast.resolve();
  await Promise.all([message, settled]);
  const result = await f.result(id);
  assert.equal(result.text, 'final');
  assert.deepEqual(await f.read('last.json'), result);
  assert.equal((await f.read('status.json')).idle, true);
});

test('questions terminate segments, stay non-idle, and resume repeated questions in fresh result files', async (t) => {
  const f = await fixture(t);
  await f.start();
  assert.ok(!f.tools.includes('ask_coordinator'));
  const first = await f.send('prompt', 'task');
  await until(() => f.sent.length === 1);
  assert.ok(f.tools.includes('ask_coordinator'));
  const tool = await ask(f, { text: 'Choose', choices: ['A', 'B'], requiresUser: true });
  assert.equal(tool.terminate, true);
  assert.deepEqual(Object.keys(tool.details.question).sort(), ['choices', 'commandId', 'id', 'requiresUser', 'text']);
  assert.ok(!f.tools.includes('ask_coordinator'));
  assert.equal(await f.read(`results/${first}.json`), null);
  await f.emit('agent_settled');
  const waiting = await f.result(first);
  assert.equal(waiting.status, 'waiting_question');
  assert.equal(waiting.text, '');
  assert.equal(waiting.question.commandId, first);
  assert.equal((await f.read('status.json')).idle, false);
  assert.deepEqual((await f.read('status.json')).question, waiting.question);
  await f.emit('agent_settled');
  assert.deepEqual(await f.read('last.json'), waiting);
  const second = await f.send('answer', 'A', answerExtra(waiting.question, 'human'));
  await until(() => f.sent.length === 2);
  assert.deepEqual(f.sent[1], ['A', { deliverAs: 'followUp', expandPromptTemplates: false }]);
  await ask(f);
  await f.emit('agent_settled');
  const next = await f.result(second);
  assert.equal(next.status, 'waiting_question');
  assert.equal(next.question.commandId, second);
  assert.equal(next.question.requiresUser, false);
  assert.notEqual(next.question.id, waiting.question.id);
  const third = await f.send('answer', 'B', answerExtra(next.question));
  await until(() => f.sent.length === 3);
  await f.emit('message_end', { message: assistant('finished') });
  await f.emit('agent_settled');
  assert.equal((await f.result(third)).status, 'completed');
  assert.deepEqual(await f.result(first), waiting);
  assert.deepEqual(await f.result(second), next);
  assert.equal((await f.result(third)).messages.length, 1);
  assert.equal((await f.read('status.json')).idle, true);
  assert.equal((await f.read('status.json')).question, undefined);
});

test('invalid answers and ordinary controls cannot destroy a pending human question', async (t) => {
  const f = await fixture(t);
  await f.start();
  const initial = await f.send('prompt', 'task');
  await until(() => f.sent.length === 1);
  const { details: { question } } = await ask(f, { text: 'Approve?', requiresUser: true });
  const early = await f.send('answer', 'yes', answerExtra(question, 'human'));
  assert.match((await f.result(early)).errorMessage, /not settled/);
  await f.emit('agent_settled');
  const waiting = await f.result(initial);
  for (const [kind, text, extra] of [
    ['answer', 'yes', { ...answerExtra(question, 'human'), questionId: randomUUID() }],
    ['answer', 'yes', { ...answerExtra(question, 'human'), questionCommandId: randomUUID() }],
    ['answer', 'yes', answerExtra(question)],
    ['answer', 'yes', answerExtra(question, 'model')],
    ['answer', '', answerExtra(question, 'human')],
    ['prompt', 'continue'], ['steer', 'do it'], ['retire'], ['unknown'],
  ]) {
    const id = await f.send(kind, text, extra);
    assert.equal((await f.result(id)).status, 'error');
    assert.deepEqual(await f.read('last.json'), waiting);
    assert.deepEqual((await f.read('status.json')).question, question);
    assert.equal((await f.read('status.json')).idle, false);
    assert.equal((await f.read('status.json')).stopped, false);
  }
  for (const source of ['interactive', 'rpc', 'extension']) {
    for (const streamingBehavior of [undefined, 'steer', 'followUp']) {
      assert.deepEqual(await f.emit('input', { text: 'bypass', source, streamingBehavior }), { action: 'handled' });
    }
  }
  assert.equal(f.sent.length, 1);
  assert.equal(f.shutdowns, 0);
  const valid = await f.send('answer', 'yes', answerExtra(question, 'human'));
  await until(() => f.sent.length === 2);
  const duplicate = envelope(f.runId, { id: valid, kind: 'answer', text: 'yes', ...answerExtra(question, 'human'), created: Date.now() });
  await atomicJson(join(f.dir, 'commands', `${valid}.json`), duplicate);
  const barrier = await f.send('answer', 'yes', answerExtra(question, 'human'));
  assert.equal((await f.result(barrier)).status, 'error');
  assert.equal(f.sent.length, 2);
  assert.equal(await f.read(`results/${valid}.json`), null);
  await f.emit('message_end', { message: assistant('done') });
  await f.emit('agent_settled');
  const done = await f.result(valid);
  await atomicJson(join(f.dir, 'commands', `${valid}.json`), duplicate);
  await f.result(await f.send('answer', 'late', answerExtra(question, 'human')));
  assert.deepEqual(await f.result(valid), done);
  assert.equal(f.sent.length, 2);
});

for (const names of [['bash', 'ask_coordinator'], ['ask_coordinator', 'write'], ['ask_coordinator', 'ask_coordinator']]) {
  test(`mixed question batch blocks every sibling before execution: ${names.join(',')}`, async (t) => {
    const f = await fixture(t);
    await f.start();
    await f.send('prompt', 'task');
    await until(() => f.sent.length === 1);
    await f.emit('message_end', { message: toolMessage(names) });
    let effects = 0;
    for (const name of names) {
      const decision = await f.emit('tool_call', { toolName: name });
      if (!decision?.block) effects++;
      assert.match(decision.reason, /only tool call/);
    }
    assert.equal(effects, 0);
    await assert.rejects(f.registered.get('ask_coordinator').execute('tool-0', { text: 'Q' }, undefined, undefined, f.ctx), /only tool call/);
    await f.emit('message_end', { message: toolMessage(['bash']) });
    assert.equal(await f.emit('tool_call', { toolName: 'bash' }), undefined);
    assert.equal((await f.read('status.json')).question, undefined);
  });
}

test('queued steering prevents a question until drained; pause immediately blocks further steering', async (t) => {
  const f = await fixture(t);
  await f.start();
  const initial = await f.send('prompt', 'task');
  await until(() => f.sent.length === 1);
  f.pending(true);
  await f.emit('message_end', { message: toolMessage() });
  assert.match((await f.emit('tool_call', { toolName: 'ask_coordinator' })).reason, /pending steering/);
  await assert.rejects(f.registered.get('ask_coordinator').execute('tool-0', { text: 'Q' }, undefined, undefined, f.ctx), /pending steering/);
  f.pending(false);
  const { details: { question } } = await ask(f);
  // The guard applies before agent_settled, not only after result publication.
  assert.equal((await f.result(await f.send('steer', 'bypass'))).status, 'error');
  assert.deepEqual(await f.emit('input', { source: 'extension', streamingBehavior: 'followUp' }), { action: 'handled' });
  assert.deepEqual(await f.emit('tool_call', { toolName: 'bash' }), { block: true, reason: 'Coordinator question pending', terminate: true });
  await f.emit('agent_settled');
  assert.equal((await f.result(initial)).question.id, question.id);
  assert.equal(f.sent.length, 1);
});

for (const reason of ['aborted', 'error']) {
  test(`a ${reason} after a question is not reported as waiting`, async (t) => {
    const f = await fixture(t);
    await f.start();
    const initial = await f.send('prompt', 'task');
    await until(() => f.sent.length === 1);
    await ask(f);
    await f.emit('message_end', { message: assistant('', reason) });
    await f.emit('agent_settled');
    assert.equal((await f.result(initial)).status, reason);
    assert.equal((await f.read('status.json')).question, undefined);
  });
}

for (const settled of [false, true]) {
  test(`abort invalidates a question (settled=${settled}) and stale answer cannot replay`, async (t) => {
    const f = await fixture(t);
    await f.start();
    const initial = await f.send('prompt', 'task');
    await until(() => f.sent.length === 1);
    const { details: { question } } = await ask(f);
    if (settled) await f.emit('agent_settled');
    assert.equal((await f.result(await f.send('abort'))).status, 'accepted');
    if (!settled) await f.emit('agent_settled');
    assert.equal((await f.read('last.json')).status, 'aborted');
    assert.equal((await f.read('status.json')).question, undefined);
    const late = await f.send('answer', 'too late', answerExtra(question));
    assert.equal((await f.result(late)).status, 'error');
    assert.equal(f.sent.length, 1);
    assert.equal((await f.result(initial)).status, settled ? 'waiting_question' : 'aborted');
  });
}

test('question tool rejects manual turns, invalid parameters, and an aborted signal', async (t) => {
  const f = await fixture(t);
  await f.start();
  const tool = f.registered.get('ask_coordinator');
  const execute = (params, signal) => tool.execute('tool-0', params, signal, undefined, f.ctx);
  await assert.rejects(execute({ text: 'Q' }), /active interactive/);
  await f.send('prompt', 'task');
  await until(() => f.sent.length === 1);
  await f.emit('message_end', { message: toolMessage() });
  for (const params of [{ text: '' }, { text: 'Q', choices: [] }, { text: 'Q', choices: [''] }, { text: 'Q', requiresUser: 'yes' }]) {
    await assert.rejects(execute(params), /Invalid coordinator question/);
  }
  await assert.rejects(execute({ text: 'Q' }, AbortSignal.abort()), /cancelled/);
  assert.equal((await f.read('status.json')).question, undefined);
});

for (const event of ['shutdown', 'lease']) {
  test(`${event} invalidates held questions without overwriting their segment result`, async (t) => {
    const f = await fixture(t, { bridge: { terminate: async () => {} } });
    await f.start();
    const initial = await f.send('prompt', 'task');
    await until(() => f.sent.length === 1);
    await ask(f);
    await f.emit('agent_settled');
    const waiting = await f.result(initial);
    if (event === 'shutdown') await f.emit('session_shutdown');
    else {
      await rm(join(f.dir, 'lease.json'));
      await until(() => f.shutdowns === 1);
    }
    assert.equal((await f.read('status.json')).stopped, true);
    assert.equal((await f.read('status.json')).question, undefined);
    assert.deepEqual(await f.result(initial), waiting);
    assert.ok(!f.tools.includes('ask_coordinator'));
  });
}

test('protocol extras cannot override identity and repeated prompt IDs never inject again', async (t) => {
  const f = await fixture(t);
  await f.start();
  const id = await f.send('prompt', 'task', { id: 'bad', runId: 'bad', version: 99, kind: 'answer', text: 'bad' });
  await until(() => f.sent.length === 1);
  assert.equal(f.sent[0][0], 'task');
  await f.emit('message_end', { message: assistant('done') });
  await f.emit('agent_settled');
  const original = await f.result(id);
  await atomicJson(join(f.dir, 'commands', `${id}.json`), envelope(f.runId, { id, kind: 'prompt', text: 'replay', created: Date.now() }));
  await f.result(await f.send('answer', 'barrier'));
  assert.deepEqual(await f.result(id), original);
  assert.equal(f.sent.length, 1);
});
