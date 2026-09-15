import test from 'node:test';
import assert from 'node:assert/strict';
import { setTimeout as delay } from 'node:timers/promises';
import { Jobs } from '../home/config/pi/extensions/subagent/jobs.ts';
import { registerTeamControls } from '../home/config/pi/extensions/subagent/controls.ts';
import { registerParentBatchGuard } from '../home/config/pi/extensions/subagent/job-results.ts';

const Type = {
  Object: properties => ({ type: 'object', properties }),
  String: options => ({ type: 'string', ...options }),
  Number: options => ({ type: 'number', ...options }),
  Optional: schema => ({ ...schema, optional: true }),
};
const StringEnum = values => ({ type: 'string', enum: values });
const deferred = () => { let resolve; const promise = new Promise(r => { resolve = r; }); return { promise, resolve }; };
async function until(fn) {
  for (let i = 0; i < 1000; i++) { const value = fn(); if (value) return value; await delay(2); }
  assert.fail('Timed out waiting for fake runner');
}
function fakePi() {
  const tools = new Map(), commands = new Map(), hooks = new Map(), messages = [];
  return { tools, commands, hooks, messages,
    registerTool: tool => tools.set(tool.name, tool),
    registerCommand: (name, command) => commands.set(name, command),
    on: (name, hook) => hooks.set(name, hook),
    sendMessage: (message, options) => messages.push({ message, options }),
  };
}
test('close all continues after cleanup debt failure and releases only confirmed members', async t => {
  const f = fixture(t), released = [], attempts = [];
  f.team.list = async () => [{ id: 'debt' }, { id: 'healthy' }];
  f.team.close = async id => { attempts.push(id); if (id === 'debt') throw new Error('denied'); };
  f.jobs.memberClosed = id => released.push(id);
  await assert.rejects(f.tool({ action: 'close', id: 'all' }), /Could not close 1.*denied/);
  assert.deepEqual(attempts, ['debt', 'healthy']);
  assert.deepEqual(released, ['healthy']);
});

function fixture(t) {
  const pi = fakePi(), resumes = [], sends = [], closes = [], uiCalls = [], notices = [];
  const member = { id: 'member', agent: 'builder', pending: false };
  let state = { idle: true }, controls;
  const jobs = new Jobs({ pollMs: 1000, onChange: snapshot => controls?.changed(snapshot) });
  const sendGate = deferred();
  const team = {
    records: new Map([[member.id, member]]),
    get(id) { assert.equal(id, member.id); return member; },
    state: async () => state,
    list: async () => [{ ...member, ...state }],
    read: async id => ({ memberId: id, status: 'completed', text: 'retained conversation' }),
    send: (id, text, options) => { sends.push({ id, text, ...options }); return sendGate.promise; },
    answer: (id, questionId, text, options) => {
      const gate = deferred(); resumes.push({ id, questionId, text, ...options, gate }); return gate.promise;
    },
    close: async id => { closes.push(id); team.records.delete(id); },
    health: async () => true,
  };
  controls = registerTeamControls(pi, () => team, Type, StringEnum, () => jobs);
  const ctx = { mode: 'tui', ui: {
    input: async title => { uiCalls.push({ method: 'input', title }); return 'typed answer'; },
    select: async (title, choices) => { uiCalls.push({ method: 'select', title, choices }); return choices[0]; },
    notify: (text, level) => notices.push({ text, level }),
  } };
  const tool = async (params, signal, context = ctx) => {
    const result = await pi.tools.get('team').execute('call', params, signal, undefined, context);
    return JSON.parse(result.content[0].text);
  };
  let sequence = 0;
  const outcome = (extra = {}) => ({ status: 'waiting_question', memberId: member.id, text: '',
    messages: [{ role: 'assistant', content: [{ type: 'text', text: 'private transcript' }] }],
    question: { id: `question-${++sequence}`, commandId: `segment-${sequence}`, text: 'Choose deployment', ...extra } });
  async function start(extra = {}) {
    const result = outcome(extra);
    const id = jobs.start({ tasks: [{ agent: member.agent }], run: async () => result,
      resume: (prior, text, source, signal) => team.answer(prior.memberId, prior.question.id, text, { source, signal }),
      cancelMember: id => team.close(id), health: id => team.health(id) });
    await jobs.wait(id);
    return { id, question: result.question };
  }
  t.after(() => { controls.reset(); jobs.shutdown(); });
  return { pi, jobs, team, controls, ctx, tool, start, outcome, resumes, sends, closes, uiCalls, notices, member, sendGate,
    setState: value => { state = value; } };
}

test('main-pane questions tool uses real registry and schema cannot override human provenance', async t => {
  const f = fixture(t);
  const { id, question } = await f.start({ requiresUser: true });
  const schema = f.pi.tools.get('team').parameters;
  assert.deepEqual(Object.keys(schema.properties).sort(), ['action', 'after', 'id', 'text']);
  assert.equal(schema.properties.after.type, 'number');
  assert.equal(schema.properties.source, undefined);
  const listed = await f.tool({ action: 'questions' });
  assert.equal(listed.members.length, 1);
  assert.equal(listed.members[0].id, question.id);
  assert.equal(listed.members[0].jobId, id);
  assert.equal(listed.members[0].memberId, f.member.id);
  await assert.rejects(f.tool({ action: 'answer', id: question.id, text: 'approved', source: 'human' }), /human answer/);
  assert.equal(f.jobs.snapshot(id).status, 'waiting_question');
  assert.equal(f.jobs.question(question.id).requiresUser, true);
  assert.deepEqual(f.resumes, []);
  assert.deepEqual(f.uiCalls, []);
  const read = await f.tool({ action: 'read', id });
  assert.equal(read.tasks[0].result.messages, undefined);
  assert.equal(f.jobs.snapshot(id).tasks[0].result.messages.length, 1, 'public redaction does not mutate registry');
});

test('wait auto-presents a human question as a selectable list', async t => {
  const f = fixture(t);
  const { id, question } = await f.start({ requiresUser: true, choices: ['staging', 'production'] });
  const ack = await f.tool({ action: 'wait', id });
  assert.deepEqual(ack, { status: 'queued', jobId: id, questionId: question.id });
  await until(() => f.resumes.length);
  assert.equal(f.resumes[0].source, 'human');
  assert.equal(f.resumes[0].text, 'staging');
  assert.deepEqual(f.uiCalls.map(call => call.method), ['select']);
  assert.deepEqual(f.uiCalls[0].choices, ['staging', 'production', 'Other answer (type text)']);
  f.resumes[0].gate.resolve({ status: 'completed', text: 'deployed', memberId: f.member.id });
  assert.equal((await f.tool({ action: 'wait', id })).status, 'completed');
});

for (const method of ['input', 'select', 'other']) test(`ask obtains ${method} in main UI and resumes with human provenance`, async t => {
  const f = fixture(t);
  const { id, question } = await f.start({ requiresUser: true, ...(method === 'input' ? {} : { choices: ['staging', 'production'] }) });
  if (method === 'other') f.ctx.ui.select = async (title, choices) => {
    f.uiCalls.push({ method: 'select', title, choices }); return choices.at(-1);
  };
  const ack = await f.tool({ action: 'ask', id: question.id });
  assert.deepEqual(ack, { status: 'queued', jobId: id, questionId: question.id });
  await until(() => f.resumes.length);
  assert.equal(f.resumes[0].source, 'human');
  assert.equal(f.resumes[0].text, method === 'select' ? 'staging' : 'typed answer');
  assert.equal(f.resumes[0].questionId, question.id);
  assert.equal(f.jobs.snapshot(id).status, 'running', 'ack is not completion');
  assert.deepEqual(f.uiCalls.map(c => c.method), method === 'other' ? ['select', 'input'] : [method]);
  assert.ok(f.uiCalls.every(c => c.title === 'builder: Choose deployment'));
  if (method !== 'input') assert.deepEqual(f.uiCalls[0].choices, ['staging', 'production', 'Other answer (type text)']);
  f.resumes[0].gate.resolve({ status: 'completed', text: 'deployed', memberId: f.member.id });
  assert.equal((await f.tool({ action: 'wait', id })).status, 'completed');
});

for (const response of [undefined, null, '', '   ']) test(`dismissed ask (${JSON.stringify(response)}) stays waiting without automatic reopen`, async t => {
  const f = fixture(t);
  const { id, question } = await f.start({ requiresUser: true });
  let inputs = 0;
  f.ctx.ui.input = async () => { inputs++; return response; };
  const result = await f.tool({ action: 'ask', id: question.id });
  assert.equal(result.status, 'waiting_question');
  assert.equal(result.questionId, question.id);
  assert.match(result.text, /Do not reopen automatically/);
  await f.tool({ action: 'wait', id });
  await delay(80);
  assert.equal(inputs, 1);
  assert.equal(f.jobs.snapshot(id).status, 'waiting_question');
  assert.ok(f.jobs.question(question.id));
  assert.deepEqual(f.resumes, []);
  assert.deepEqual(f.pi.messages, []);
});

for (const method of ['input', 'select', 'other', 'select-other-late']) test(`cancelled ${method} dialog forwards signal and ignores late human response`, async t => {
  const f = fixture(t);
  const { id, question } = await f.start({ requiresUser: true,
    ...(method === 'input' ? {} : { choices: ['staging', 'production'] }) });
  const controller = new AbortController(), gate = deferred(), calls = [];
  const answer = t.mock.method(f.jobs, 'answer');
  f.ctx.ui.select = async (title, choices, options) => {
    calls.push({ method: 'select', options });
    return method === 'other' ? choices.at(-1) : gate.promise;
  };
  f.ctx.ui.input = async (title, placeholder, options) => {
    calls.push({ method: 'input', options });
    return gate.promise;
  };
  const asking = f.tool({ action: 'ask', id: question.id }, controller.signal);
  const rejected = assert.rejects(asking, /cancelled.*still pending/);
  await until(() => calls.length === (method === 'other' ? 2 : 1));
  assert.ok(calls.every(call => call.options?.signal === controller.signal));
  controller.abort();
  gate.resolve(method === 'select-other-late' ? 'Other answer (type text)' : 'staging');
  await rejected;
  await delay(0);
  assert.deepEqual(calls.map(call => call.method), method === 'other' ? ['select', 'input'] : [method === 'input' ? 'input' : 'select']);
  assert.equal(answer.mock.callCount(), 0);
  assert.deepEqual(f.resumes, []);
  assert.equal(f.jobs.snapshot(id).status, 'waiting_question');
  assert.equal(f.jobs.question(question.id).id, question.id);
});

test('ask refuses noninteractive contexts and slash answer is human input', async t => {
  const f = fixture(t);
  const { id, question } = await f.start({ requiresUser: true });
  await assert.rejects(f.tool({ action: 'ask', id: question.id }, undefined, { ...f.ctx, mode: 'rpc' }), /main interactive Pi pane/);
  assert.deepEqual(f.uiCalls, []);
  await f.pi.commands.get('team').handler(`answer ${question.id} approved by user`, f.ctx);
  await until(() => f.resumes.length);
  assert.equal(f.resumes[0].source, 'human');
  assert.equal(f.resumes[0].text, 'approved by user');
  assert.equal(f.jobs.snapshot(id).status, 'running');
  assert.deepEqual(f.notices, []);
  assert.equal(JSON.parse(f.pi.messages[0].message.content).status, 'queued');
  assert.deepEqual(f.pi.messages[0].options, { triggerTurn: true, deliverAs: 'followUp' });
});

test('answer acknowledges once, wait yields the next question, and later notifications are deduplicated', async t => {
  const f = fixture(t);
  const { id, question } = await f.start();
  const first = await f.tool({ action: 'wait', id });
  const ack = await f.tool({ action: 'answer', id: question.id, text: 'staging' });
  assert.deepEqual(ack, { status: 'queued', jobId: id, questionId: question.id });
  assert.deepEqual(await f.tool({ action: 'answer', id: question.id, text: 'staging' }), ack);
  await until(() => f.resumes.length);
  assert.equal(f.resumes.length, 1);
  assert.equal(f.resumes[0].source, 'coordinator');
  assert.equal(f.jobs.snapshot(id).status, 'running');
  assert.equal((await f.tool({ action: 'wait', id, after: first.revision })).status, 'running');
  const next = f.outcome();
  f.resumes[0].gate.resolve(next);
  await f.jobs.wait(id);
  await until(() => f.pi.messages.length === 1);
  assert.match(f.pi.messages[0].message.content, new RegExp(next.question.id));
  assert.deepEqual(f.pi.messages[0].options, { triggerTurn: true, deliverAs: 'followUp' });
  f.controls.changed(f.jobs.snapshot(id));
  f.controls.changed(f.jobs.snapshot(id));
  await delay(80);
  assert.equal(f.pi.messages.length, 1);
  const second = await f.tool({ action: 'wait', id });
  assert.equal(second.tasks[0].result.question.id, next.question.id);
  await f.tool({ action: 'answer', id: next.question.id, text: 'continue' });
  await until(() => f.resumes.length === 2);
  f.resumes[1].gate.resolve({ status: 'completed', text: 'finished', memberId: f.member.id });
  await f.jobs.wait(id);
  await until(() => f.pi.messages.length === 2);
  assert.match(f.pi.messages[1].message.content, /finished/);
  f.controls.changed(f.jobs.snapshot(id));
  await delay(80);
  assert.equal(f.pi.messages.length, 2);
});

test('a question consumed by wait suppresses its delayed notification', async t => {
  const f = fixture(t);
  const { id, question } = await f.start();
  await f.tool({ action: 'wait', id });
  await f.tool({ action: 'answer', id: question.id, text: 'continue' });
  await until(() => f.resumes.length);
  const next = f.outcome();
  f.resumes[0].gate.resolve(next);
  const result = await f.tool({ action: 'wait', id });
  assert.equal(result.tasks[0].result.question.id, next.question.id);
  await delay(80);
  assert.deepEqual(f.pi.messages, []);
});

test('team close reconciles four waiting owners and does not strand later jobs', async t => {
  const f = fixture(t);
  const ids = Array.from({ length: 4 }, (_, index) => f.jobs.start({ tasks: [{ agent: `worker-${index}` }],
    run: async () => ({ status: 'waiting_question', memberId: `owned-${index}`, text: '',
      question: { id: `owned-question-${index}`, text: 'Continue?' } }) }));
  await Promise.all(ids.map(id => f.jobs.wait(id)));
  const queued = f.jobs.start({ tasks: [{ agent: 'replacement' }], run: async () => ({ status: 'completed', text: 'started' }) });
  assert.equal(f.jobs.snapshot(queued).tasks[0].state, 'queued');
  for (let index = 0; index < 4; index++) {
    assert.equal((await f.tool({ action: 'close', id: `owned-${index}` })).status, 'closed');
    assert.equal(f.jobs.snapshot(ids[index]).tasks[0].state, 'aborted');
    assert.throws(() => f.jobs.answer(`owned-question-${index}`, 'late'), /stale/);
    await f.tool({ action: 'close', id: `owned-${index}` });
  }
  assert.equal(f.jobs.questions().length, 0);
  assert.equal((await f.jobs.wait(queued, { timeoutMs: 1000 })).status, 'completed');
  const fresh = f.jobs.start({ mode: 'parallel', tasks: Array.from({ length: 4 }, () => ({ agent: 'fresh' })),
    run: async () => ({ status: 'completed', text: 'free slot' }) });
  assert.equal((await f.jobs.wait(fresh, { timeoutMs: 1000 })).status, 'completed');
});

test('asynchronous cleanup failure notifies once after terminal cancel acknowledgement with ownership', async t => {
  const f = fixture(t);
  const cleanup = deferred();
  f.team.close = async () => { await cleanup.promise; throw new Error('owned pane refused close'); };
  const { id } = await f.start();
  const ack = await f.tool({ action: 'cancel', id });
  assert.equal(ack.status, 'aborted');
  assert.equal(ack.tasks[0].cleanupError, undefined);
  await delay(80);
  assert.equal(f.pi.messages.length, 0, 'cancel acknowledgement already made terminal status visible');
  cleanup.resolve();
  await until(() => f.pi.messages.length === 1);
  const { message, options } = f.pi.messages[0];
  assert.equal(message.customType, 'team-job');
  assert.ok(message.content.includes(f.member.id));
  assert.match(message.content, /owned pane refused close/);
  assert.equal(options.triggerTurn, true);
  assert.equal(options.deliverAs, 'followUp');
  f.controls.changed(f.jobs.snapshot(id));
  await f.tool({ action: 'cancel', id });
  await delay(100);
  assert.equal(f.pi.messages.length, 1);
});

test('cancel invalidates waiting answers and closes member once', async t => {
  const f = fixture(t);
  const { id, question } = await f.start();
  const result = await f.tool({ action: 'cancel', id });
  assert.equal(result.status, 'aborted');
  assert.deepEqual(f.closes, [f.member.id]);
  assert.equal(f.jobs.questions().length, 0);
  await assert.rejects(f.tool({ action: 'answer', id: question.id, text: 'late' }), /stale/);
  await f.tool({ action: 'cancel', id });
  assert.deepEqual(f.closes, [f.member.id]);
  assert.deepEqual(f.resumes, []);
});

test('send followup owns execution beyond the initial wait and resumes same member', async t => {
  const f = fixture(t);
  const controller = new AbortController();
  const sending = f.tool({ action: 'send', id: f.member.id, text: 'followup' }, controller.signal);
  await until(() => f.sends.length);
  assert.equal(f.sends[0].id, f.member.id);
  assert.equal(f.sends[0].text, 'followup');
  assert.notEqual(f.sends[0].signal, controller.signal);
  const result = f.outcome();
  f.sendGate.resolve(result);
  const waiting = await sending;
  assert.equal(waiting.status, 'waiting_question');
  controller.abort();
  assert.equal(f.sends[0].signal.aborted, false, 'returned wait must detach tool signal');
  await f.tool({ action: 'answer', id: result.question.id, text: 'use retained context' });
  await until(() => f.resumes.length);
  assert.equal(f.resumes[0].id, f.member.id);
  assert.equal(f.resumes[0].source, 'coordinator');
  assert.equal(f.resumes[0].signal, f.sends[0].signal);
  f.resumes[0].gate.resolve({ status: 'completed', text: 'followup done', memberId: f.member.id });
  const completed = await f.tool({ action: 'wait', id: waiting.jobId });
  assert.equal(completed.tasks[0].result.text, 'followup done');
  assert.equal(f.sends.length, 1);
});

test('send rejects waiting/busy members before starting a job; aborting active wait cancels owned signal', async t => {
  const f = fixture(t);
  for (const state of [{ idle: false, question: { id: 'held' } }, { idle: false }]) {
    f.setState(state);
    await assert.rejects(f.tool({ action: 'send', id: f.member.id, text: 'no' }), /question pending|busy/);
  }
  assert.equal(f.jobs.jobs.size, 0);
  assert.deepEqual(f.sends, []);
  f.setState({ idle: true });
  const controller = new AbortController();
  const sending = f.tool({ action: 'send', id: f.member.id, text: 'cancel me' }, controller.signal);
  const rejected = assert.rejects(sending, /aborted/);
  await until(() => f.sends.length);
  controller.abort();
  await rejected;
  assert.equal(f.sends[0].signal.aborted, true);
  assert.equal([...f.jobs.jobs.keys()].map(id => f.jobs.snapshot(id).status)[0], 'aborted');
  f.sendGate.resolve({ status: 'completed', memberId: f.member.id, text: 'late' });
  await until(() => f.closes.length);
  assert.deepEqual(f.closes, [f.member.id]);
});

for (const blocker of [{ name: 'subagent', arguments: { tasks: [{ agent: 'scout' }, { agent: 'builder' }] } },
  ...['send', 'wait', 'ask'].map(action => ({ name: 'team', arguments: { action } }))]) {
  test(`parent guard blocks every sibling of ${blocker.name} ${blocker.arguments.action ?? 'tasks'} before effects`, () => {
    const pi = fakePi();
    let active = true;
    registerParentBatchGuard(pi, () => active);
    const calls = [{ type: 'toolCall', id: 'before', name: 'bash', arguments: { command: 'touch file' } },
      { type: 'toolCall', id: 'blocking', ...blocker },
      { type: 'toolCall', id: 'after', name: 'team', arguments: { action: 'answer' } }];
    pi.hooks.get('message_end')({ message: { role: 'assistant', content: calls } });
    const effects = [];
    for (const call of calls) {
      const decision = pi.hooks.get('tool_call')({ toolCallId: call.id, toolName: call.name });
      assert.equal(decision?.block, true);
      if (!decision?.block) effects.push(call.id);
    }
    assert.deepEqual(effects, []);
    active = false;
    assert.equal(pi.hooks.get('tool_call')({ toolCallId: 'blocking' }), undefined);
    active = true;
    pi.hooks.get('message_end')({ message: { role: 'assistant', content: [calls[1]] } });
    assert.equal(pi.hooks.get('tool_call')({ toolCallId: 'blocking' }), undefined, 'one subagent may parallelize internally with tasks');
    pi.hooks.get('message_end')({ message: { role: 'assistant', content: [calls[0], calls[2]] } });
    assert.equal(pi.hooks.get('tool_call')({ toolCallId: 'before' }), undefined);
    assert.equal(pi.hooks.get('tool_call')({ toolCallId: 'after' }), undefined);
  });
}
