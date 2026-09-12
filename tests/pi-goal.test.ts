import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import test from 'node:test';
import {
  COMPLETE_MARKER, BLOCKED_MARKER, WAITING_MARKER, MAX_CONTINUATIONS, MAX_AUDITS,
  activeGoalPrompt, auditorPrompt, auditTextFromEvents, claimFromAssistant,
  continuationPrompt, createAuditRunner, createGoalExtension, escapeXml,
  parseAuditReport, parseGoalCommand, requirementsFor, validateObjective,
} from '../home/config/pi/extensions/goal.ts';

const contract = { id: 'goal-1', revision: 1, auditId: 'audit-1', objective: 'Ship login\nCover every error case' };
function report(goal = contract, status = 'verified') {
  return JSON.stringify({ auditId: goal.auditId, goalId: goal.id, revision: goal.revision,
    verdict: status === 'verified' ? 'PASS' : status === 'failed' ? 'FAIL' : 'UNVERIFIED',
    requirements: requirementsFor(goal.objective).map(({ id }) => ({ id, status, evidence: ['src/login.js:1: inspected implementation'] })),
  });
}
const accepted = async ({ contract }) => ({ text: report(contract) });
function harness({ runAudit = accepted, hasUI = true } = {}) {
  const events = {}, commands = {}, entries = [], messages = [], notices = [], statuses = [];
  const h = { entries, messages, notices, statuses, confirmed: true, idle: true, pending: false };
  const ctx = { hasUI, cwd: '/tmp/work', model: { provider: 'test', id: 'mock' },
    isIdle: () => h.idle, hasPendingMessages: () => h.pending,
    sessionManager: { getBranch: () => entries },
    ui: { notify: (text, level) => notices.push([text, level]), setStatus: (...args) => statuses.push(args),
      confirm: async () => h.confirmed, editor: async () => h.edited },
  };
  createGoalExtension({ runAudit })({ on: (name, handler) => { events[name] = handler; },
    registerCommand: (name, command) => { commands[name] = command; },
    appendEntry: (customType, data) => entries.push({ type: 'custom', customType, data: structuredClone(data) }),
    sendMessage: (message, options) => { if (h.sendError) throw Error('send failed'); messages.push({ message, options }); },
    getThinkingLevel: () => 'medium',
  });
  h.command = args => commands.goal.handler(args, ctx);
  h.emit = (name, event = {}) => events[name](event, ctx);
  h.completions = prefix => commands.goal.getArgumentCompletions(prefix);
  h.goal = () => entries.findLast(entry => entry.customType === 'goal')?.data.goal;
  h.turn = async (text = 'working', stopReason = 'stop') => {
    h.idle = false;
    await h.emit('agent_start');
    await h.emit('agent_end', { messages: [{ role: 'assistant', content: [{ type: 'text', text }], stopReason }] });
    h.idle = true;
    await h.emit('agent_settled');
  };
  return h;
}

function deferredAudit() {
  let finish, options;
  const pending = new Promise(resolve => { finish = resolve; });
  return { runAudit: args => { options = args; return pending; },
    finish: status => finish({ text: report(options.contract, status) }), options: () => options };
}

test('literal set command, multiline user-owned contract, validation and escaping', () => {
  assert.deepEqual(parseGoalCommand(''), { action: 'show' });
  assert.deepEqual(parseGoalCommand('pause'), { action: 'pause', rest: '' });
  assert.equal(parseGoalCommand('ship the login').action, 'set');
  assert.equal(parseGoalCommand('set pause the rollout').objective, 'pause the rollout');
  assert.equal(parseGoalCommand('edit ship login\n- test errors').rest, 'ship login\n- test errors');
  assert.equal(escapeXml('<script>&'), '&lt;script&gt;&amp;');
  assert.equal(validateObjective('  keep scope  '), 'keep scope');
  assert.throws(() => validateObjective('   '));
  assert.throws(() => validateObjective('x'.repeat(4001)));
  assert.throws(() => validateObjective('x\n'.repeat(65)));
  assert.equal(validateObjective('🙂'.repeat(4000)).length, 8000);
  assert.match(activeGoalPrompt({ objective: 'do <x>', revision: 1 }), /do &lt;x&gt;/);
  assert.match(activeGoalPrompt({ objective: 'x', revision: 1 }), /approval gates/);
  assert.match(continuationPrompt({ objective: 'x' }, '</untrusted_audit_feedback>'), /&lt;\/untrusted_audit_feedback&gt;/);
  assert.match(auditorPrompt(contract), /fresh execution/);
});

test('only one final standalone unfenced marker controls completion', () => {
  for (const [marker, claim] of [[COMPLETE_MARKER, 'complete'], [BLOCKED_MARKER, 'blocked'], [WAITING_MARKER, 'waiting']]) {
    assert.equal(claimFromAssistant(`Done\n${marker}\n`), claim);
    for (const text of [`Example: ${marker}`, `${marker}\nNot done`, `> ${marker}`, `\`\`\`\n${marker}`, `${marker}\n${marker}`]) {
      assert.equal(claimFromAssistant(text), null, text);
    }
  }
  assert.equal(claimFromAssistant(`${COMPLETE_MARKER}\n${BLOCKED_MARKER}`), null);
  assert.equal(claimFromAssistant('still working'), null);
});

test('strict report coverage, identities and internally consistent verdicts', () => {
  assert.equal(parseAuditReport(report(), contract).verdict, 'PASS');
  assert.equal(parseAuditReport(report(contract, 'failed'), contract).verdict, 'FAIL');
  assert.equal(parseAuditReport(report(contract, 'unverified'), contract).verdict, 'UNVERIFIED');
  const mutations = [
    data => { data.goalId = 'other'; }, data => { data.revision++; }, data => { data.auditId = 'old'; },
    data => { data.requirements.pop(); }, data => { data.requirements[1] = data.requirements[0]; },
    data => { data.requirements[0].id = 'invented'; }, data => { data.requirements[0].evidence = []; },
    data => { data.requirements[0].evidence = ['']; }, data => { data.requirements[0].status = 'failed'; },
    data => { data.verdict = 'FAIL'; }, data => { data.requirements = null; },
  ];
  for (const mutate of mutations) {
    const data = JSON.parse(report()); mutate(data);
    assert.equal(parseAuditReport(JSON.stringify(data), contract).verdict, 'UNVERIFIED');
  }
  for (const text of ['PASS', 'explanation\nPASS\nFAIL\nMissing verification', `\`\`\`json\n${report()}\n\`\`\``, 'x'.repeat(64001), null]) {
    assert.equal(parseAuditReport(text, contract).verdict, 'UNVERIFIED');
  }
});

test('continues only after host settlement; no goal means no injected prompt', async () => {
  const h = harness();
  assert.equal(await h.emit('before_agent_start', { systemPrompt: 'base' }), undefined);
  await h.command('keep scope');
  assert.equal(h.goal().status, 'active');
  assert.equal(h.goal().continuations, 0);
  assert.match((await h.emit('before_agent_start', { systemPrompt: 'base' })).systemPrompt, /^base/);
  h.messages.length = 0;
  await h.emit('agent_start'); h.idle = false;
  await h.emit('agent_end', { messages: [{ role: 'assistant', content: 'working', stopReason: 'stop' }] });
  assert.equal(h.messages.length, 0);
  h.idle = true; await h.emit('agent_settled');
  assert.equal(h.messages.length, 1);
  assert.equal(h.messages[0].message.customType, 'goal-continuation');
  assert.deepEqual(h.messages[0].options, { triggerTurn: true });
  await h.emit('agent_settled');
  assert.equal(h.messages.length, 1, 'duplicate settled event cannot schedule another turn');
});

test('unmet requirements feed back; only a matching independent audit completes', async () => {
  let audits = 0;
  const h = harness({ runAudit: async ({ contract, provider, model, thinking }) => {
    assert.equal(provider, 'test'); assert.equal(model, 'mock'); assert.equal(thinking, 'medium');
    return { text: report(contract, ++audits === 1 ? 'failed' : 'verified') };
  } });
  await h.command('ship login');
  await h.turn(`claim\n${COMPLETE_MARKER}`);
  assert.equal(h.goal().status, 'active');
  assert.match(h.messages.at(-1).message.content, /untrusted_audit_feedback/);
  await h.turn(`claim\n${COMPLETE_MARKER}`);
  assert.equal(h.goal().status, 'complete');
  const count = h.messages.length;
  await h.command('pause'); await h.command('resume'); await h.turn();
  assert.equal(h.messages.length, count);
  assert.equal(h.goal().status, 'complete');
});

for (const action of ['pause', 'clear', 'edit changed\nacceptance criteria', 'replacement objective', 'session_tree', 'session_start', 'session_shutdown', 'input', 'agent_start']) {
  test(`late audit cannot cross ${action}`, async () => {
    const d = deferredAudit(), h = harness({ runAudit: d.runAudit });
    await h.command('old objective');
    const pending = h.command('verify');
    assert.equal(h.goal().status, 'auditing');
    if (['session_tree', 'session_start', 'session_shutdown', 'input', 'agent_start'].includes(action)) await h.emit(action);
    else await h.command(action);
    assert.equal(d.options().signal.aborted, true);
    const before = structuredClone(h.goal());
    d.finish('verified'); await pending;
    assert.deepEqual(h.goal(), before);
    assert.notEqual(h.goal()?.status, 'complete');
    if (action === 'replacement objective') assert.equal(h.goal().objective, action);
  });
}

test('new audit is not invalidated by an older result finishing', async () => {
  const requests = [];
  const h = harness({ runAudit: options => new Promise(resolve => requests.push({ options, resolve })) });
  await h.command('one'); const first = h.command('verify');
  await h.command('pause'); const second = h.command('verify');
  requests[0].resolve({ text: report(requests[0].options.contract) }); await first;
  assert.equal(h.goal().status, 'auditing');
  requests[1].resolve({ text: report(requests[1].options.contract) }); await second;
  assert.equal(h.goal().status, 'complete');
});

test('Esc, errors, impasses, and approval waits stop automatic turns', async () => {
  for (const [text, reason, expected] of [['stopped', 'aborted', 'paused'], ['error', 'error', 'blocked'],
    [`Need consent\n${WAITING_MARKER}`, 'stop', 'paused'], [`No access\n${BLOCKED_MARKER}`, 'stop', 'blocked']]) {
    const h = harness(); await h.command('goal'); const count = h.messages.length;
    await h.turn(text, reason);
    assert.equal(h.goal().status, expected); assert.equal(h.messages.length, count);
    await h.emit('input', { source: 'interactive', text: 'yes' });
    assert.equal(h.goal().status, expected, 'ordinary input must not invent resume approval');
  }
});

test('typed human-required team question pauses; ordinary tool output does not', async () => {
  const h = harness(); await h.command('ship');
  const details = { job: { tasks: [{ result: { question: { requiresUser: true } } }] } };
  await h.emit('tool_result', { toolName: 'read', details }); assert.equal(h.goal().status, 'active');
  await h.emit('tool_result', { toolName: 'subagent', details }); assert.equal(h.goal().status, 'paused');
});

test('budgets are bounded and only explicit resume/start resets them', async () => {
  const h = harness(); await h.command('work');
  for (let i = 0; i < MAX_CONTINUATIONS + 5; i++) await h.turn();
  assert.equal(h.messages.length, MAX_CONTINUATIONS + 1);
  assert.equal(h.goal().status, 'limited');
  assert.equal(h.goal().continuations, MAX_CONTINUATIONS);
  await h.command('edit changed'); assert.equal(h.goal().continuations, MAX_CONTINUATIONS);
  await h.command('resume'); assert.equal(h.goal().continuations, 0); assert.equal(h.goal().status, 'active');
});

test('repeated failed audits are capped, including explicit verify', async () => {
  let count = 0;
  const h = harness({ runAudit: async ({ contract }) => { count++; return { text: report(contract, 'failed') }; } });
  await h.command('ship');
  for (let i = 0; i < MAX_AUDITS + 2; i++) await h.turn(`claim\n${COMPLETE_MARKER}`);
  await h.command('verify');
  assert.equal(count, MAX_AUDITS); assert.equal(h.goal().status, 'limited');
  await h.command('edit new'); await h.command('verify'); assert.equal(count, MAX_AUDITS);
  await h.command('resume'); assert.equal(h.goal().audits, 0);
});

test('missing evidence and infrastructure failures park without repair loops', async () => {
  for (const runAudit of [async ({ contract }) => ({ text: report(contract, 'unverified') }),
    async () => ({ error: 'timeout' }), async () => ({ text: 'PASS' }), async () => { throw Error('boom'); }]) {
    const h = harness({ runAudit }); await h.command('ship'); const count = h.messages.length;
    await h.turn(`claim\n${COMPLETE_MARKER}`);
    assert.equal(h.goal().status, 'unverified'); assert.equal(h.messages.length, count);
    assert.ok(h.goal().auditReport);
  }
});

test('manual verify of a paused goal never restarts implementation on FAIL', async () => {
  const h = harness({ runAudit: async ({ contract }) => ({ text: report(contract, 'failed') }) });
  await h.command('ship'); await h.command('pause'); const count = h.messages.length;
  await h.command('verify'); assert.equal(h.goal().status, 'unverified'); assert.equal(h.messages.length, count);
});

test('pending input and busy work suppress audits and continuation', async () => {
  let calls = 0;
  const h = harness({ runAudit: async () => { calls++; return {}; } }); await h.command('work');
  h.pending = true; const count = h.messages.length;
  await h.turn(`claim\n${COMPLETE_MARKER}`); await h.command('verify');
  assert.equal(calls, 0); assert.equal(h.messages.length, count);
  h.pending = false; h.idle = false; await h.command('verify'); assert.equal(calls, 0);
  await h.command('new objective'); assert.equal(h.goal().status, 'paused');
});

test('old implementation turn cannot complete newly set goal', async () => {
  let count = 0;
  const h = harness({ runAudit: async options => { count++; return accepted(options); } });
  await h.command('old'); await h.emit('agent_start');
  await h.command('new');
  await h.emit('agent_end', { messages: [{ role: 'assistant', content: `done\n${COMPLETE_MARKER}`, stopReason: 'stop' }] });
  await h.emit('agent_settled'); assert.equal(count, 0); assert.equal(h.goal().status, 'active');
});

test('state snapshots are immutable, branch-local, restored without automatic consent', async () => {
  const h = harness(); await h.command('one'); const initial = structuredClone(h.entries[0]);
  await h.command('pause'); assert.deepEqual(h.entries[0], initial);
  await h.command('resume'); const count = h.messages.length;
  await h.emit('session_start'); await h.command('status');
  assert.match(h.notices.at(-1)[0], /paused/); assert.equal(h.messages.length, count);
  h.entries.splice(1); await h.emit('session_tree'); await h.command('status');
  assert.match(h.notices.at(-1)[0], /one/);
  h.entries.length = 0; await h.emit('session_start');
  assert.equal(await h.emit('before_agent_start', { systemPrompt: 'base' }), undefined);
  h.entries.push({ type: 'custom', customType: 'goal', data: { version: 1, goal: { id: 'legacy', objective: 'old', status: 'complete' } } });
  await h.emit('session_start'); await h.command('status');
  assert.match(h.notices.at(-1)[0], /legacy completion claims require a new audit/);
});

test('context drops old revisions, cleared goals and duplicate continuations', async () => {
  const h = harness(); await h.command('first'); const old = h.messages[0].message;
  await h.command('second'); const current = h.messages.at(-1).message;
  const user = { role: 'user', content: 'hi' };
  const messages = [old, user, current, current];
  assert.deepEqual((await h.emit('context', { messages })).messages, [user, current]);
  await h.command('clear'); assert.deepEqual((await h.emit('context', { messages })).messages, [user]);
});

test('cancelled editor/replacement, command typos and no-UI replacement do not mutate contract', async () => {
  const h = harness(); await h.command('ship'); const original = structuredClone(h.goal());
  h.confirmed = false; await h.command('other');
  await h.command('edit'); await h.command('resume extra');
  assert.deepEqual(h.goal(), original);
  h.confirmed = true; h.edited = 'new\ncriteria'; await h.command('edit');
  assert.equal(h.goal().revision, 2); assert.equal(h.goal().objective, h.edited); assert.equal(h.goal().status, 'paused');
  assert.ok(h.completions('pa').some(item => item.value === 'pause'));
  assert.equal(h.completions('zz'), null);
  const headless = harness({ hasUI: false }); await headless.command('first'); await headless.command('second');
  assert.equal(headless.goal().objective, 'first');
  await headless.command('clear'); await headless.command('second'); assert.equal(headless.goal().objective, 'second');
});

test('failed send pauses rather than spending an unbounded retry loop', async () => {
  const h = harness(); h.sendError = true; await h.command('ship'); assert.equal(h.goal().status, 'paused');
});

function events(text = report(), stopReason = 'stop') {
  return JSON.stringify({ type: 'agent_end', messages: [
    { role: 'toolResult', toolName: 'read', content: [{ type: 'text', text: 'source' }], isError: false },
    { role: 'assistant', content: [{ type: 'text', text }], stopReason },
  ] }) + '\n';
}

test('transport accepts only final successful read-only agent output', () => {
  assert.equal(auditTextFromEvents(events()).text, report());
  for (const text of ['PASS', events() + events(), events(report(), 'aborted'), events(report(), 'error'),
    events().replace('"toolName":"read"', '"toolName":"bash"'), events().replace('"isError":false', '"isError":true'),
    JSON.stringify({ type: 'message_end', message: { role: 'assistant', content: report() } })]) {
    assert.ok(auditTextFromEvents(text).error);
  }
});

function runnerFor(script, settings = {}, inspect = () => {}) {
  return createAuditRunner({ timeoutMs: 2000, killGraceMs: 40, ...settings,
    spawnProcess: (command, args, options) => { inspect(command, args, options); return spawn(process.execPath, ['-e', script], options); },
  });
}
const auditOptions = () => ({ cwd: process.cwd(), contract, provider: 'test', model: 'mock', thinking: 'low', signal: new AbortController().signal });

test('real child transport preserves offline policy, restricts tools and keeps goal off argv', async () => {
  const script = `process.stdin.resume(); process.stdin.on('end', () => process.stdout.write(${JSON.stringify(events())}));`;
  const run = runnerFor(script, {}, (command, args, options) => {
    assert.equal(command, 'pi'); assert.equal(args[args.indexOf('--tools') + 1], 'read,grep,find,ls');
    assert.ok(args.includes('--no-extensions')); assert.ok(args.includes('--no-context-files'));
    assert.ok(!args.join(' ').includes(contract.objective)); assert.equal(options.env.PI_OFFLINE, process.env.PI_OFFLINE);
  });
  assert.equal((await run(auditOptions())).text, report());
});

test('nonzero exit cannot launder a PASS; stderr is not exposed', async () => {
  const result = await runnerFor(`process.stdout.write(${JSON.stringify(events())}); process.stderr.write('sensitive diagnostic'); process.exitCode = 1;`)(auditOptions());
  assert.match(result.error, /unsuccessfully/); assert.doesNotMatch(result.error, /sensitive/);
});

for (const kind of ['timeout', 'abort', 'overflow']) {
  test(`bounded process cleanup on ${kind}, including a child ignoring TERM`, { timeout: 5000 }, async () => {
    const controller = new AbortController(); let child;
    const runner = createAuditRunner({ timeoutMs: kind === 'timeout' ? 200 : 2000, killGraceMs: 40, maxBytes: 1000,
      spawnProcess: (_command, _args, options) => {
        child = spawn(process.execPath, ['-e', `process.on('SIGTERM',()=>{}); process.stdin.resume(); ${kind === 'overflow' ? "process.stdout.write('x'.repeat(2000));" : ''} setInterval(()=>{},1000);`], options);
        return child;
      },
    });
    const pending = runner({ ...auditOptions(), signal: controller.signal });
    if (kind === 'abort') setTimeout(() => controller.abort(), 100);
    const result = await pending;
    assert.match(result.error, kind === 'timeout' ? /timed out/ : kind === 'abort' ? /cancelled/ : /limit/);
    if (child.exitCode === null && child.signalCode === null) await new Promise(resolve => child.once('exit', resolve));
    assert.throws(() => process.kill(child.pid, 0), { code: 'ESRCH' });
  });
}

test('already cancelled and failed spawn do not start an audit', async () => {
  let spawns = 0;
  const run = createAuditRunner({ spawnProcess: () => { spawns++; throw Error('secret'); } });
  const controller = new AbortController(); controller.abort();
  assert.match((await run({ ...auditOptions(), signal: controller.signal })).error, /cancelled/); assert.equal(spawns, 0);
  assert.equal((await run(auditOptions())).error, 'Auditor failed to start.');
});
