import assert from 'node:assert/strict';
import test from 'node:test';
import install from '../home/config/pi/extensions/context-window.ts';

function harness({ entries = [], model, tokens = 1000, hasUI = true } = {}) {
  const base = model ?? Object.freeze({
    provider: 'openai-codex', id: 'gpt-6-astra', contextWindow: 272000,
    api: 'openai-codex-responses', maxTokens: 128000, cost: { input: 0 },
  });
  const events = {}, commands = {}, notices = [], statuses = [], calls = [];
  const h = { base, entries, notices, statuses, calls, choice: 1, confirmed: true, idle: true, auth: true };
  const ctx = {
    model: base, hasUI, isIdle: () => h.idle,
    getContextUsage: () => ({ tokens }),
    sessionManager: { getBranch: () => h.entries },
    ui: {
      notify: (...args) => notices.push(args),
      setStatus: (...args) => statuses.push(args),
      select: async (title, options) => {
        h.menu = { title, options };
        return options[h.choice];
      },
      confirm: async (...args) => { h.confirmation = args; return h.confirmed; },
    },
  };
  install({
    getThinkingLevel: () => h.thinking ?? 'high',
    setThinkingLevel: level => { h.thinking = level; },
    on: (name, handler) => { events[name] = handler; },
    registerCommand: (name, command) => { commands[name] = command; },
    appendEntry: (customType, data) => h.entries.push({ type: 'custom', customType, data }),
    setModel: async model => {
      calls.push(model);
      if (h.error) throw new Error('model unavailable');
      if (!h.auth) return false;
      const previousModel = ctx.model;
      ctx.model = model;
      h.thinking = 'medium';
      await events.model_select?.({ model, previousModel, source: 'set' }, ctx);
      return true;
    },
  });
  h.ctx = ctx;
  h.command = (args = '') => commands.context.handler(args, ctx);
  h.emit = (name, event = {}) => events[name]?.(event, ctx);
  h.completions = prefix => commands.context.getArgumentCompletions(prefix);
  return h;
}

test('picker offers three presets, marks current, and changes only the session model', async () => {
  const h = harness();
  await h.command();
  assert.equal(h.menu.options.length, 3);
  assert.match(h.menu.options[0], /272k.*current/i);
  assert.match(h.menu.options[1], /500k/);
  assert.match(h.menu.options[2], /872k/);
  assert.equal(h.ctx.model.contextWindow, 500000);
  assert.equal(h.base.contextWindow, 272000);
  assert.equal(h.ctx.model.id, h.base.id);
  assert.equal(h.ctx.model.maxTokens, 128000);
  assert.equal(h.thinking, 'high');
  assert.deepEqual(h.ctx.model.cost, h.base.cost);
  assert.equal(h.entries.length, 1);
  assert.match(h.notices.at(-1)[0], /compaction/i);
});

test('direct presets and argument completion', async () => {
  const h = harness();
  await h.command('maximum');
  assert.equal(h.ctx.model.contextWindow, 872000);
  await h.command('272k');
  assert.equal(h.ctx.model.contextWindow, 272000);
  assert.ok(h.completions('ex').some(item => item.value === 'extended'));
  await h.command('1000000');
  assert.equal(h.ctx.model.contextWindow, 272000);
  assert.match(h.notices.at(-1)[0], /usage/i);
});

test('Escape and unchanged choice are no-ops', async () => {
  const h = harness();
  h.choice = -1;
  await h.command();
  h.choice = 0;
  await h.command();
  assert.equal(h.calls.length, 0);
  assert.equal(h.entries.length, 0);
});

test('shrinking below usage requires confirmation and does not itself compact', async () => {
  const h = harness({ tokens: 600000 });
  await h.command('maximum');
  h.confirmed = false;
  await h.command('lean');
  assert.equal(h.ctx.model.contextWindow, 872000);
  assert.match(h.confirmation.join(' '), /600000/);
  h.confirmed = true;
  await h.command('lean');
  assert.equal(h.ctx.model.contextWindow, 272000);
});

test('shrinking with unknown usage also requires confirmation', async () => {
  const h = harness({ tokens: null });
  await h.command('maximum');
  h.confirmed = false;
  await h.command('lean');
  assert.equal(h.ctx.model.contextWindow, 872000);
  assert.ok(h.confirmation);
});

test('saved budget survives reload/resume and model switching without recursive changes', async () => {
  const h = harness();
  await h.command('extended');
  const resumed = harness({ entries: h.entries });
  await resumed.emit('session_start', { reason: 'resume' });
  assert.equal(resumed.ctx.model.contextWindow, 500000);
  assert.equal(resumed.calls.length, 1);
  resumed.ctx.model = resumed.base;
  await resumed.emit('model_select');
  assert.equal(resumed.ctx.model.contextWindow, 500000);
  await resumed.emit('session_start', { reason: 'reload' });
  assert.equal(resumed.ctx.model.contextWindow, 500000);
  assert.equal(resumed.entries.length, 1);
});

test('same-model reselection is repaired before prompt compaction or opening the menu', async () => {
  const h = harness();
  await h.command('maximum');
  h.ctx.model = h.base;
  await h.emit('input');
  assert.equal(h.ctx.model.contextWindow, 872000);
  h.ctx.model = h.base;
  h.choice = -1;
  await h.command();
  assert.equal(h.ctx.model.contextWindow, 872000);
  assert.match(h.menu.options[2], /current/);
});

test('new sessions and branches without a saved choice use lean', async () => {
  const h = harness();
  await h.emit('session_start', { reason: 'new' });
  assert.equal(h.ctx.model.contextWindow, 272000);
  await h.command('maximum');
  h.entries = [];
  await h.emit('session_tree');
  assert.equal(h.ctx.model.contextWindow, 272000);
});

test('invalid persisted choices are ignored and latest valid choice wins', async () => {
  const h = harness();
  await h.command('extended');
  await h.command('maximum');
  h.entries.push({ ...h.entries.at(-1), data: { ...h.entries.at(-1).data, contextWindow: 2000000 } });
  h.ctx.model = h.base;
  await h.emit('session_start');
  assert.equal(h.ctx.model.contextWindow, 872000);
});

test('other models, providers and missing models are not modified', async () => {
  for (const model of [
    { provider: 'openai', id: 'gpt-6-astra', contextWindow: 1050000 },
    { provider: 'openai-codex', id: 'gpt-5.5', contextWindow: 272000 },
  ]) {
    const h = harness({ model });
    await h.command('maximum');
    await h.emit('model_select');
    assert.equal(h.calls.length, 0);
    assert.match(h.notices[0][0], /only/i);
    assert.equal(h.statuses.at(-1)[1], undefined);
  }
  const h = harness();
  h.ctx.model = undefined;
  await h.command();
  assert.equal(h.calls.length, 0);
});

test('busy and non-interactive commands are refused', async () => {
  const h = harness();
  h.idle = false;
  await h.command('maximum');
  assert.equal(h.calls.length, 0);
  assert.match(h.notices.at(-1)[0], /idle/i);
  const noUI = harness({ hasUI: false });
  await noUI.command('maximum');
  assert.equal(noUI.calls.length, 0);
});

test('model changes while the menu is open do not apply stale choices', async () => {
  const h = harness();
  h.ctx.ui.select = async (_title, options) => {
    h.ctx.model = { ...h.base, id: 'other' };
    return options[1];
  };
  await h.command();
  assert.equal(h.calls.length, 0);
});

test('auth failures and exceptions do not save state, and allow retry', async () => {
  const h = harness();
  h.auth = false;
  await h.command('extended');
  assert.equal(h.entries.length, 0);
  assert.equal(h.ctx.model.contextWindow, 272000);
  h.auth = true;
  h.error = true;
  await h.command('extended');
  assert.equal(h.entries.length, 0);
  h.error = false;
  await h.command('maximum');
  assert.equal(h.ctx.model.contextWindow, 872000);
  assert.equal(h.entries.length, 1);
});
