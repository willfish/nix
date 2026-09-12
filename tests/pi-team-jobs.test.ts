import test from 'node:test';
import assert from 'node:assert/strict';
import { Jobs } from '../home/config/pi/extensions/subagent/jobs.ts';

const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
const turn = () => new Promise(resolve => setImmediate(resolve));
const tasks = count => Array.from({ length: count }, (_, index) => ({ agent: `agent-${index}` }));
const done = text => ({ status: 'completed', text });
const question = (id, requiresUser = false) => ({ status: 'waiting_question', text: '', memberId: `member-${id}`,
  question: { id, text: `Question ${id}?`, requiresUser, commandId: `command-${id}` } });
const registry = (t, options) => {
  const jobs = new Jobs(options);
  t.after(() => jobs.shutdown());
  return jobs;
};

test('first question yields while slow sibling continues and is retained', async t => {
  const jobs = registry(t);
  const slow = deferred();
  const first = deferred();
  const id = jobs.start({ mode: 'parallel', tasks: tasks(2), run: (_, index) => index ? slow.promise : first.promise,
    resume: async () => done('answer result') });
  const pending = jobs.wait(id);
  first.resolve(question('first'));
  const snapshot = await pending;
  assert.equal(snapshot.status, 'waiting_question');
  assert.equal(snapshot.tasks[1].state, 'running');
  slow.resolve(done('slow result'));
  await turn();
  assert.equal(jobs.snapshot(id).tasks[1].result.text, 'slow result');
  assert.equal(jobs.answer('first', 'yes').status, 'queued');
  const result = await jobs.wait(id);
  assert.equal(result.status, 'completed');
  assert.deepEqual(result.tasks.map(task => task.result.text), ['answer result', 'slow result']);
});

test('four waiting tasks retain global slots and all eight queued tasks survive', async t => {
  const jobs = registry(t);
  const starts = [];
  const id = jobs.start({ mode: 'parallel', tasks: tasks(8), run: async (_, index) => {
    starts.push(index); return question(`q${index}`);
  }, resume: async outcome => done(outcome.question.id) });
  await turn();
  assert.deepEqual(starts, [0, 1, 2, 3]);
  assert.equal(jobs.questions().length, 4);
  assert.equal(jobs.snapshot(id).tasks.filter(task => task.state === 'queued').length, 4);
  for (let index = 0; index < 8; index++) {
    jobs.answer(`q${index}`, 'go');
    await turn();
  }
  assert.deepEqual(starts, [0, 1, 2, 3, 4, 5, 6, 7]);
  assert.equal((await jobs.wait(id)).status, 'completed');
});

test('capacity applies across jobs, including simultaneous answer resumes', async t => {
  const jobs = registry(t, { capacity: 2 });
  const resumed = deferred();
  const first = jobs.start({ mode: 'parallel', tasks: tasks(2), run: async (_, index) => question(`global${index}`),
    resume: () => resumed.promise });
  await turn();
  let ran = false;
  const second = jobs.start({ tasks: tasks(1), run: async () => { ran = true; return done('second'); } });
  await turn();
  assert.equal(ran, false);
  jobs.answer('global0', 'go');
  jobs.answer('global1', 'go');
  await turn();
  assert.equal(ran, false);
  resumed.resolve(done('first'));
  await jobs.wait(first);
  assert.equal((await jobs.wait(second)).status, 'completed');
  assert.equal(ran, true);
});

for (const questionsReady of [true, false]) test(`queued job wait promptly exposes four cross-job questions (already waiting: ${questionsReady})`, async t => {
  const jobs = registry(t);
  const gate = deferred();
  jobs.start({ mode: 'parallel', tasks: tasks(4), run: async (_, index) => {
    await gate.promise;
    return question(`blocker-${index}`);
  } });
  if (questionsReady) { gate.resolve(); await turn(); }
  let launched = false;
  const id = jobs.start({ tasks: tasks(1), run: async () => { launched = true; return done('new job'); } });
  let timer;
  try {
    const waiting = jobs.wait(id);
    if (!questionsReady) gate.resolve();
    const snapshot = await Promise.race([waiting, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('Queued wait did not yield within 1 second')), 1000);
    })]);
    assert.equal(snapshot.status, 'running');
    assert.equal(snapshot.tasks[0].state, 'queued');
    assert.ok(snapshot.blockedByQuestions.length > 0);
    await turn();
    assert.deepEqual(jobs.snapshot(id).blockedByQuestions.sort(), tasks(4).map((_, index) => `blocker-${index}`));
    if (questionsReady) assert.equal(snapshot.blockedByQuestions.length, 4);
    assert.equal(launched, false);
  } finally { clearTimeout(timer); }
});

test('chains keep previous final output through repeated questions and stop on failure', async t => {
  const jobs = registry(t);
  const seen = [];
  const id = jobs.start({ mode: 'chain', tasks: tasks(4), run: async (_, index, previous) => {
    seen.push([index, previous]);
    if (index === 0) return done('initial');
    if (index === 1) return question('chain1');
    throw new Error('step failed');
  }, resume: async outcome => outcome.question.id === 'chain1' ? question('chain2') : done('final step one') });
  await jobs.wait(id);
  assert.deepEqual(seen, [[0, ''], [1, 'initial']]);
  jobs.answer('chain1', 'intermediate answer');
  assert.equal((await jobs.wait(id)).tasks[1].result.question.id, 'chain2');
  assert.deepEqual(seen, [[0, ''], [1, 'initial']]);
  jobs.answer('chain2', 'last answer');
  const snapshot = await jobs.wait(id);
  assert.equal(snapshot.status, 'error');
  assert.deepEqual(seen, [[0, ''], [1, 'initial'], [2, 'final step one']]);
  assert.equal(snapshot.tasks[3].state, 'aborted');
});

test('question validation, human provenance, stale answers and replay idempotency', async t => {
  const jobs = registry(t);
  let injections = 0;
  const resumed = deferred();
  const id = jobs.start({ tasks: tasks(1), run: async () => question('human', true),
    resume: (outcome, text, source) => {
      injections++;
      assert.equal(outcome.memberId, 'member-human');
      assert.equal(text, 'yes');
      assert.equal(source, 'human');
      return resumed.promise;
    } });
  await jobs.wait(id);
  assert.equal(jobs.question('human').jobId, id);
  assert.throws(() => jobs.answer('unknown', 'yes'), /stale/);
  assert.throws(() => jobs.answer('human', 'yes'), /human/);
  assert.throws(() => jobs.answer('human', 'yes', { source: 'model' }), /source/);
  assert.throws(() => jobs.answer('human', '  ', { source: 'human' }), /required/);
  const ack = jobs.answer('human', 'yes', { source: 'human' });
  assert.equal(ack.status, 'queued');
  assert.deepEqual(jobs.answer('human', 'yes', { source: 'human' }), ack);
  assert.throws(() => jobs.answer('human', 'no', { source: 'human' }), /differently/);
  assert.equal(jobs.question('human'), undefined);
  await turn();
  assert.equal(injections, 1);
  resumed.resolve(done('finished'));
  await jobs.wait(id);
  assert.deepEqual(jobs.answer('human', 'yes', { source: 'human' }), ack);
});

test('wait abort listener detaches after yield and later attached wait abort owns cancellation', async t => {
  const jobs = registry(t);
  const cancelled = [];
  let workerSignal;
  const id = jobs.start({ tasks: tasks(1), run: async (_, index, previous, signal) => {
    workerSignal = signal; return question('detach');
  }, cancelMember: memberId => cancelled.push(memberId) });
  const caller = new AbortController();
  const waiting = await jobs.wait(id, { signal: caller.signal });
  caller.abort();
  assert.equal(workerSignal.aborted, false);
  assert.equal(jobs.snapshot(id).status, 'waiting_question');
  const attached = new AbortController();
  const pending = jobs.wait(id, { after: waiting.revision, signal: attached.signal });
  attached.abort();
  assert.equal((await pending).status, 'aborted');
  assert.equal(workerSignal.aborted, true);
  assert.deepEqual(cancelled, ['member-detach']);
  assert.equal(jobs.questions().length, 0);
  assert.throws(() => jobs.answer('detach', 'late'), /stale/);
});

test('timeouts detach without cancelling, cursor waits for change, snapshots are isolated', async t => {
  const jobs = registry(t);
  const worker = deferred();
  const id = jobs.start({ tasks: tasks(1), run: () => worker.promise });
  const caller = new AbortController();
  const early = await jobs.wait(id, { timeoutMs: 1, signal: caller.signal });
  assert.equal(early.status, 'running');
  caller.abort();
  assert.equal(jobs.snapshot(id).status, 'running');
  const next = jobs.wait(id, { after: early.revision });
  worker.resolve(question('cursor'));
  const waiting = await next;
  waiting.tasks[0].result.question.text = 'mutated';
  assert.equal(jobs.question('cursor').text, 'Question cursor?');
});

test('cancellation and shutdown ignore late resolution, reject stale replay, and capture cleanup errors', async t => {
  const jobs = registry(t);
  const late = deferred();
  let signal;
  const id = jobs.start({ tasks: tasks(1), run: async () => question('cancel'),
    resume: (_, text, source, ownedSignal) => { signal = ownedSignal; return late.promise; },
    cancelMember: async () => { throw new Error('cleanup failed'); } });
  await jobs.wait(id);
  jobs.answer('cancel', 'yes');
  await turn();
  jobs.shutdown();
  assert.equal(signal.aborted, true);
  assert.equal(jobs.snapshot(id).status, 'aborted');
  assert.throws(() => jobs.answer('cancel', 'yes'), /stale/);
  late.resolve(question('too-late'));
  await turn();
  assert.equal(jobs.question('too-late'), undefined);
  assert.throws(() => jobs.start({ tasks: tasks(1), run: () => done('no') }), /shut down/);
});

test('sync, async and invalid worker errors settle without unhandled rejection', async t => {
  const jobs = registry(t, { onChange: async () => { throw new Error('notification failed'); } });
  const id = jobs.start({ mode: 'parallel', tasks: tasks(4), run: (_, index) => {
    if (index === 0) throw new Error('sync');
    if (index === 1) return Promise.reject(new Error('async'));
    if (index === 2) return { status: 'nonsense' };
    return { status: 'waiting_question', question: {} };
  } });
  const snapshot = await jobs.wait(id);
  assert.equal(snapshot.status, 'error');
  assert.deepEqual(snapshot.tasks.map(task => task.result.text), ['sync', 'async', 'Invalid worker outcome', 'Invalid question outcome']);
  await turn();
});

test('health failure invalidates questions and stops chain without a human wait timeout', async t => {
  const jobs = registry(t, { pollMs: 5 });
  const health = deferred();
  const started = deferred();
  let polls = 0;
  const cancelled = [];
  const id = jobs.start({ mode: 'chain', tasks: tasks(2), run: async () => question('health'),
    health: () => { polls++; started.resolve(); return health.promise; },
    cancelMember: memberId => cancelled.push(memberId) });
  await jobs.wait(id);
  // An explicit timeout keeps the test process alive while the registry's idle timer is unref'ed.
  const pending = jobs.wait(id, { after: jobs.snapshot(id).revision, timeoutMs: 1000 });
  await started.promise;
  await new Promise(resolve => setTimeout(resolve, 15));
  assert.equal(polls, 1);
  assert.equal(jobs.snapshot(id).status, 'waiting_question');
  health.reject(new Error('member disappeared'));
  const failed = await pending;
  assert.equal(failed.status, 'error');
  assert.equal(failed.tasks[1].state, 'aborted');
  assert.equal(jobs.questions().length, 0);
  assert.deepEqual(cancelled, ['member-health']);
  assert.throws(() => jobs.answer('health', 'yes'), /stale/);
});

test('aborted wait retains capacity until late discovered members are cleaned up', async t => {
  const jobs = registry(t, { capacity: 1 });
  const late = deferred();
  const cleaned = [];
  const first = jobs.start({ tasks: tasks(1), run: () => late.promise, cancelMember: memberId => cleaned.push(memberId) });
  await turn();
  const second = jobs.start({ tasks: tasks(1), run: async () => done('queued job') });
  const caller = new AbortController();
  caller.abort();
  assert.equal((await jobs.wait(first, { signal: caller.signal })).status, 'aborted');
  await turn();
  assert.equal(jobs.snapshot(second).tasks[0].state, 'queued');
  late.resolve(question('late-member'));
  await turn();
  assert.deepEqual(cleaned, ['member-late-member']);
  assert.equal((await jobs.wait(second)).status, 'completed');
  assert.equal(jobs.question('late-member'), undefined);
});

test('terminal cleanup outcomes retain a slot through cleanup flight and debt until manual reconciliation', async t => {
  const jobs = registry(t, { capacity: 1 });
  const cleanup = deferred();
  const closed = [];
  const id = jobs.start({ tasks: tasks(1), run: async () => ({ status: 'error', text: 'task timed out',
    memberId: 'orphaned-member', cleanupError: 'internal close failed' }),
    cancelMember: memberId => { closed.push(memberId); return cleanup.promise; } });
  assert.equal((await jobs.wait(id)).status, 'error');
  await turn(); // Worker flight has settled; only the cleanup flight still occupies capacity.
  const queued = jobs.start({ tasks: tasks(1), run: async () => done('replacement') });
  await turn();
  assert.deepEqual(closed, ['orphaned-member']);
  assert.equal(jobs.snapshot(id).tasks[0].cleanupError, undefined);
  assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
  cleanup.reject(new Error('retry close failed'));
  await turn();
  assert.equal(jobs.snapshot(id).tasks[0].result.memberId, 'orphaned-member');
  assert.match(jobs.snapshot(id).tasks[0].cleanupError, /retry close failed/);
  assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
  jobs.memberClosed('unrelated-member');
  await turn();
  assert.equal(jobs.snapshot(queued).tasks[0].state, 'queued');
  jobs.memberClosed('orphaned-member');
  jobs.memberClosed('orphaned-member');
  assert.equal(jobs.snapshot(id).tasks[0].cleanupError, undefined);
  assert.equal((await jobs.wait(queued, { timeoutMs: 1000 })).status, 'completed');
});

test('resume failures and recycled question IDs fail rather than replaying', async t => {
  const jobs = registry(t);
  const first = jobs.start({ tasks: tasks(1), run: async () => question('recycled'),
    resume: async () => question('recycled') });
  await jobs.wait(first);
  jobs.answer('recycled', 'yes');
  assert.equal((await jobs.wait(first)).tasks[0].result.text, 'Question ID already used');
  const second = jobs.start({ tasks: tasks(1), run: async () => question('resume-error'),
    resume: () => { throw new Error('resume failed'); } });
  await jobs.wait(second);
  jobs.answer('resume-error', 'yes');
  assert.equal((await jobs.wait(second)).tasks[0].result.text, 'resume failed');
});

test('negative health result fails a waiting member', async t => {
  const jobs = registry(t, { pollMs: 1 });
  const id = jobs.start({ tasks: tasks(1), run: async () => question('missing'), health: () => false });
  const waiting = await jobs.wait(id);
  const failed = await jobs.wait(id, { after: waiting.revision, timeoutMs: 1000 });
  assert.equal(failed.status, 'error');
  assert.match(failed.tasks[0].result.text, /unavailable/);
});

test('late unhealthy check cannot invalidate an already resumed segment', async t => {
  const jobs = registry(t, { pollMs: 1 });
  const health = deferred();
  const started = deferred();
  const id = jobs.start({ tasks: tasks(1), run: async () => question('health-race'), resume: async () => done('safe'),
    health: () => { started.resolve(); return health.promise; } });
  await jobs.wait(id);
  const keepAlive = setTimeout(() => {}, 1000);
  try {
    await started.promise;
    jobs.answer('health-race', 'yes');
    await jobs.wait(id);
    health.resolve(false);
    await turn();
    assert.equal(jobs.snapshot(id).status, 'completed');
  } finally { clearTimeout(keepAlive); }
});
