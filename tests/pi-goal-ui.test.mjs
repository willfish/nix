import assert from 'node:assert/strict';
import test from 'node:test';
import { AUDIT_CARD_TYPE, AUDIT_UPDATE_TYPE, auditCardLines, auditProgress,
  createAuditCardComponent, createGoalExtension, safeUiText } from '../home/config/pi/extensions/goal.js';

const card = () => ({ auditId: 'audit', phase: 'running', revision: 2, elapsedSeconds: 3,
  model: 'test/model', activity: 'read: src/login.js', tools: [{ id: 't1', tool: 'read', path: 'src/login.js', state: 'done' }],
  requirements: [{ id: 'r1', text: 'Handle errors' }], report: '' });
const failedReport = JSON.stringify({ requirements: [{ id: 'r1', status: 'failed', evidence: ['src/login.js:4: missing error handling'] }] });

test('quiet collapsed card, expanded metadata, terminal evidence and safe plain text', () => {
  const value = card();
  assert.deepEqual(auditCardLines(value, false), ['Audit running | 3s | revision 2', 'test/model | read: src/login.js']);
  assert.match(auditCardLines(value, true).join('\n'), /done: read src\/login.js/);
  value.phase = 'FAIL'; value.report = failedReport;
  assert.match(auditCardLines(value, false).join('\n'), /0 verified, 1 failed, 0 unverified/);
  assert.doesNotMatch(auditCardLines(value, false).join('\n'), /missing error handling/);
  assert.match(auditCardLines(value, true).join('\n'), /r1 failed: Handle errors/);
  assert.match(auditCardLines(value, true).join('\n'), /missing error handling/);
  assert.equal(safeUiText('\x1b]52;c;secret\x07\u202etext\n'), ' ]52;c;secret  text ');
  assert.equal(safeUiText('x'.repeat(1000)).length, 240);
});

test('renderer reads live state and toggles only on primary mouse clicks', () => {
  const value = card();
  class Text { constructor(text) { this.text = text; } render() { return this.text.split('\n'); } }
  const renderer = createAuditCardComponent(() => value, false, { fg: (_color, text) => text },
    { Text, keyHint: (_key, hint) => `Ctrl+O: ${hint}` });
  assert.doesNotMatch(renderer.render(80).join('\n'), /done: read/);
  assert.equal(renderer.handleMouse({ type: 'move', button: 'left' }), undefined);
  assert.equal(renderer.handleMouse({ type: 'click', button: 'right' }), undefined);
  assert.deepEqual(renderer.handleMouse({ type: 'click', button: 'left' }), { handled: true, render: true });
  assert.match(renderer.render(80).join('\n'), /done: read/);
  value.phase = 'PASS'; value.elapsedSeconds = 7;
  assert.match(renderer.render(80).join('\n'), /Audit PASS \| 7s/);
  renderer.invalidate();
  assert.match(renderer.render(80).join('\n'), /Audit PASS/);
  const keyboardExpanded = createAuditCardComponent(() => value, true, {}, { Text, keyHint: () => '' });
  assert.match(keyboardExpanded.render(30).join('\n'), /done: read/);
});

test('only allowlisted inspection metadata reaches the card', () => {
  assert.equal(auditProgress({ type: 'message_update', text: 'secret reasoning' }), null);
  assert.equal(auditProgress({ type: 'tool_execution_start', toolName: 'bash', args: { command: 'secret' } }), null);
  assert.deepEqual(auditProgress({ type: 'tool_execution_start', toolCallId: '1', toolName: 'grep', args: { path: 'src', pattern: 'secret search term' } }),
    { id: '1', tool: 'grep', path: 'src', state: 'running' });
  assert.deepEqual(auditProgress({ type: 'tool_execution_end', toolCallId: '1', toolName: 'read', isError: true, result: { content: 'secret contents' } }),
    { id: '1', tool: 'read', path: '.', state: 'error' });
});

function harness() {
  const entries = [], events = {}, commands = {}, renderers = {}, requests = [], statuses = [];
  const ctx = { hasUI: true, isIdle: () => true, hasPendingMessages: () => false, cwd: '/tmp',
    model: { provider: 'test', id: 'mock' }, sessionManager: { getBranch: () => entries },
    ui: { setStatus: (...args) => statuses.push(args), notify() {}, confirm: async () => true } };
  createGoalExtension({ runAudit: options => new Promise(resolve => requests.push({ options, resolve })),
    renderCard: getCard => ({ snapshot: () => structuredClone(getCard()) }),
  })({ on: (name, fn) => { events[name] = fn; }, registerCommand: (name, value) => { commands[name] = value; },
    registerEntryRenderer: (name, renderer) => { renderers[name] = renderer; },
    appendEntry: (customType, data) => entries.push({ type: 'custom', customType, data: structuredClone(data) }), sendMessage() {},
  });
  return { entries, requests, statuses, command: args => commands.goal.handler(args, ctx),
    emit: event => events[event]({}, ctx),
    render: entry => renderers[AUDIT_CARD_TYPE](entry, { expanded: false }, {}),
  };
}

test('one live anchor, bounded activity, durable terminal card and no tick after completion', async t => {
  t.mock.timers.enable({ apis: ['Date', 'setInterval'], now: 1000 });
  const h = harness(); await h.command('set Verify source'); const pending = h.command('verify');
  const anchor = h.entries.find(entry => entry.customType === AUDIT_CARD_TYPE);
  const component = h.render(anchor);
  assert.equal(component.snapshot().phase, 'running');
  const request = h.requests[0];
  for (let i = 0; i < 20; i++) request.options.onProgress({ id: String(i), tool: 'read', path: `src/${i}.js`, state: 'running' });
  request.options.onProgress({ id: '19', tool: 'read', path: '.', state: 'done' });
  assert.equal(component.snapshot().tools.length, 12);
  assert.equal(component.snapshot().tools.at(-1).path, 'src/19.js');
  assert.equal(h.entries.filter(entry => entry.customType === AUDIT_CARD_TYPE).length, 1);
  t.mock.timers.tick(2000);
  assert.equal(component.snapshot().elapsedSeconds, 2);
  const c = request.options.contract;
  request.resolve({ text: JSON.stringify({ auditId: c.auditId, goalId: c.id, revision: c.revision, verdict: 'PASS',
    requirements: [{ id: 'r1', status: 'verified', evidence: ['src/19.js:1: verified'] }] }) });
  await pending;
  assert.equal(component.snapshot().phase, 'PASS');
  const terminal = h.entries.findLast(entry => entry.customType === AUDIT_UPDATE_TYPE);
  assert.equal(terminal.data.phase, 'PASS');
  const statusCount = h.statuses.length;
  t.mock.timers.tick(5000); assert.equal(h.statuses.length, statusCount);
  await h.emit('session_start'); assert.equal(h.render(anchor).snapshot().phase, 'PASS');
});

test('cancellation closes the card and stale progress cannot overwrite it', async () => {
  const h = harness(); await h.command('set Verify source'); const pending = h.command('verify');
  const anchor = h.entries.find(entry => entry.customType === AUDIT_CARD_TYPE);
  await h.command('pause');
  assert.equal(h.render(anchor).snapshot().phase, 'cancelled');
  h.requests[0].options.onProgress({ id: 'late', tool: 'read', path: 'late', state: 'done' });
  h.requests[0].resolve({ text: 'PASS' }); await pending;
  assert.equal(h.render(anchor).snapshot().activity, 'Stopped; no verdict accepted');
  assert.equal(h.entries.filter(entry => entry.customType === AUDIT_UPDATE_TYPE).length, 1);
});

test('interrupted restoration has no invented verdict or writes into another branch', async () => {
  const h = harness(); await h.command('set Verify source'); const pending = h.command('verify');
  const anchor = h.entries.find(entry => entry.customType === AUDIT_CARD_TYPE);
  const count = h.entries.length;
  await h.emit('session_tree'); assert.equal(h.entries.length, count);
  assert.equal(h.render(anchor).snapshot().phase, 'interrupted');
  assert.match(h.render(anchor).snapshot().report, /No verdict recorded/);
  h.requests[0].resolve({ error: 'cancelled' }); await pending;
  assert.equal(h.entries.length, count);
});
