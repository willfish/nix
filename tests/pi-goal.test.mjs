import assert from 'node:assert/strict';
import test from 'node:test';
import {
  COMPLETE_MARKER,
  activeGoalPrompt,
  claimFromAssistant,
  continuationPrompt,
  createGoalExtension,
  escapeXml,
  parseAuditReport,
  parseGoalCommand,
  validateObjective,
} from '../home/config/pi/extensions/goal.js';

function harness({ runAudit, hasUI = true } = {}) {
  const events = {};
  const commands = {};
  const notices = [];
  const statuses = [];
  const messages = [];
  const entries = [];
  const h = {
    notices,
    statuses,
    messages,
    entries,
    confirmed: true,
    idle: true,
    thinking: 'medium',
    model: { provider: 'openai-codex', id: 'gpt-6-astra' },
  };
  const ctx = {
    hasUI,
    cwd: '/tmp/work',
    model: h.model,
    isIdle: () => h.idle,
    hasPendingMessages: () => false,
    sessionManager: { getBranch: () => h.entries },
    ui: {
      notify: (text, level = 'info') => notices.push([text, level]),
      setStatus: (name, value) => statuses.push([name, value]),
      confirm: async () => h.confirmed,
      editor: async (_title, value) => h.edited ?? value,
      theme: { fg: (_color, text) => text },
    },
  };
  createGoalExtension({ runAudit })({
    on: (name, handler) => { events[name] = handler; },
    registerCommand: (name, command) => { commands[name] = command; },
    appendEntry: (customType, data) => h.entries.push({ type: 'custom', customType, data }),
    sendMessage: (message, options) => messages.push({ message, options }),
    getThinkingLevel: () => h.thinking,
  });
  h.ctx = ctx;
  h.command = args => commands.goal.handler(args, ctx);
  h.emit = (name, event = {}) => events[name](event, ctx);
  h.completions = prefix => commands.goal.getArgumentCompletions(prefix);
  return h;
}

test('command parsing and untrusted objective escaping', () => {
  assert.deepEqual(parseGoalCommand(''), { action: 'show' });
  assert.deepEqual(parseGoalCommand('pause'), { action: 'pause', rest: '' });
  assert.equal(parseGoalCommand('ship the login').action, 'set');
  assert.equal(escapeXml('<script>&'), '&lt;script&gt;&amp;');
  assert.equal(validateObjective('  keep the agents honest  '), 'keep the agents honest');
  assert.throws(() => validateObjective('   '));
  assert.match(activeGoalPrompt({ objective: 'do <x>', status: 'active' }), /untrusted_objective/);
  assert.match(continuationPrompt({ objective: 'do <x>' }, 'missing tests'), /Independent audit/);
});

test('sets a goal, injects it, and continues after ordinary turns', async () => {
  const h = harness({ runAudit: async () => ({ verdict: 'FAIL', report: 'unused' }) });
  await h.command('keep the agents honest');
  assert.equal(h.entries.at(-1).data.goal.status, 'active');
  assert.match(h.notices.at(-1)[0], /keep the agents honest/);
  const prompt = await h.emit('before_agent_start', { systemPrompt: 'base' });
  assert.match(prompt.systemPrompt, /keep the agents honest/);
  h.messages.length = 0;
  await h.emit('agent_start');
  await h.emit('agent_end', { messages: [{ role: 'assistant', content: 'working' }] });
  assert.equal(h.messages.length, 1);
  assert.equal(h.messages[0].message.customType, 'goal-continuation');
  assert.equal(h.messages[0].options.triggerTurn, true);
});

test('completion claims stay open until an isolated auditor passes', async () => {
  let audits = 0;
  const h = harness({
    runAudit: async ({ objective }) => {
      audits += 1;
      assert.equal(objective, 'ship login');
      return audits === 1
        ? { verdict: 'FAIL', report: 'FAIL\nno tests' }
        : { verdict: 'PASS', report: 'PASS\ntests cover login' };
    },
  });
  await h.command('ship login');
  await h.emit('agent_end', {
    messages: [{ role: 'assistant', content: `done ${COMPLETE_MARKER}` }],
  });
  assert.equal(h.entries.at(-1).data.goal.status, 'active');
  assert.match(h.notices.at(-1)[0], /rejected/);
  await h.command('verify');
  assert.equal(h.entries.at(-1).data.goal.status, 'complete');
  assert.equal(audits, 2);
});

test('pause, resume, clear, and argument completion', async () => {
  const h = harness({ runAudit: async () => ({ verdict: 'PASS', report: 'PASS' }) });
  await h.command('finish the docs');
  await h.emit('agent_start');
  await h.command('pause');
  assert.equal(h.entries.at(-1).data.goal.status, 'paused');
  h.messages.length = 0;
  await h.emit('agent_end', { messages: [{ role: 'assistant', content: 'still going' }] });
  assert.equal(h.messages.length, 0);
  await h.command('resume');
  assert.equal(h.entries.at(-1).data.goal.status, 'active');
  assert.equal(h.messages.length, 1);
  await h.command('clear');
  assert.equal(h.entries.at(-1).data.goal, null);
  assert.ok(h.completions('pa').some(item => item.value === 'pause'));
});

test('reconstruct restores the latest session goal', async () => {
  const h = harness({ runAudit: async () => ({ verdict: 'PASS', report: 'PASS' }) });
  h.entries.push({
    type: 'custom',
    customType: 'goal',
    data: { goal: { id: 'g1', objective: 'old', status: 'paused' } },
  });
  await h.emit('session_start');
  await h.command('');
  assert.match(h.notices.at(-1)[0], /old/);
  assert.match(h.notices.at(-1)[0], /paused/);
});

test('audit parser fails closed', () => {
  assert.equal(parseAuditReport('PASS\nlooks good').verdict, 'PASS');
  assert.equal(parseAuditReport('the tests passed').verdict, 'FAIL');
  assert.equal(claimFromAssistant(`ok ${COMPLETE_MARKER}`), 'complete');
  assert.equal(claimFromAssistant('still working'), null);
});
