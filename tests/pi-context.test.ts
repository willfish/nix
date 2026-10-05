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
    modelRegistry: { find: (provider, id) => h.registry.get(`${provider}/${id}`) },
    getContextUsage: () => ({ tokens }),
    sessionManager: { getBranch: () => h.entries },
    ui: {
      notify: (...args) => notices.push(args),
      setStatus: (...args) => statuses.push(args),
      select: async (title, options) => {
        h.menu = { title, options };
        return options[typeof h.choice === 'string' ? options.findIndex(option => option.startsWith(h.choice)) : h.choice];
      },
      confirm: async (...args) => { h.confirmation = args; return h.confirmed; },
    },
  };
  h.registry = new Map([[`${base.provider}/${base.id}`, base]]);
  h.choice = 'Extended:';
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

test('picker offers model-aware presets, marks current, and changes only the session model', async () => {
  const h = harness();
  await h.command();
  assert.equal(h.menu.options.length, 6);
  assert.ok(h.menu.options.some(option => /272k.*current/i.test(option)));
  assert.ok(h.menu.options.some(option => /Extended: 500k/.test(option)));
  assert.ok(h.menu.options.some(option => /Maximum: 872k/.test(option)));
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

test('Escape does not save a choice and selecting the current default records a reset', async () => {
  const h = harness();
  h.choice = -1;
  await h.command();
  h.choice = 'Default:';
  await h.command();
  assert.equal(h.calls.length, 0);
  assert.equal(h.entries.length, 1);
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
  assert.ok(h.menu.options.some(option => /872k.*current/.test(option)));
});

test('new sessions and branches without a saved choice use the registered default', async () => {
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

test('all registered models support budgets bounded by their own window', async () => {
  for (const model of [
    { provider: 'openai', id: 'gpt-6-astra', contextWindow: 1050000 },
    { provider: 'openai-codex', id: 'gpt-5.5', contextWindow: 272000 },
    { provider: 'xai', id: 'grok-4.7', contextWindow: 500000 },
    { provider: 'relay', id: 'qwen', contextWindow: 65536 },
    { provider: 'andromeda', id: 'qwen', contextWindow: 131072 },
    { provider: 'opencode-go', id: 'glm-5.3-flash', contextWindow: 200000 },
    { provider: 'future', id: 'unknown', contextWindow: 128000 },
  ]) {
    const h = harness({ model });
    await h.command('64k');
    assert.equal(h.ctx.model.contextWindow, 64000);
    assert.equal(model.contextWindow, h.base.contextWindow);
    await h.command(String(model.contextWindow + 1));
    assert.equal(h.ctx.model.contextWindow, 64000);
    assert.match(h.notices.at(-1)[0], /usage/i);
    await h.emit('model_select');
    assert.equal(h.ctx.model.contextWindow, 64000);
    await h.command('default');
    assert.equal(h.ctx.model.contextWindow, model.contextWindow);
    assert.deepEqual(h.entries.at(-1).data, {
      provider: model.provider, model: model.id, contextWindow: model.contextWindow, choice: 'default',
    });
  }
});

test('missing and invalid registry metadata are not guessed from a session override', async () => {
  for (const contextWindow of [undefined, NaN, Infinity, 0, -1, 1000.5, Number.MAX_SAFE_INTEGER + 1]) {
    const h = harness({ model: { provider: 'local', id: 'test', contextWindow } });
    await h.command('64k');
    await h.emit('model_select');
    assert.equal(h.calls.length, 0);
    assert.match(h.notices[0][0], /valid registered/);
    assert.equal(h.statuses.at(-1)[1], undefined);
  }
  const unregistered = harness();
  unregistered.registry.clear();
  await unregistered.command();
  assert.equal(unregistered.calls.length, 0);
  const h = harness();
  h.ctx.model = undefined;
  await h.command();
  assert.equal(h.calls.length, 0);
});

test('numeric budgets accept exact token counts and k suffixes, rejecting malformed and tiny values', async () => {
  const h = harness({ model: { provider: 'local', id: 'test', contextWindow: 131072 } });
  for (const [argument, expected] of [
    ['65536', 65536], ['65.536k', 65536], ['64.001k', 64001], ['64.002k', 64002],
    ['100k', 100000], ['131.072K', 131072],
  ]) {
    await h.command(argument);
    assert.equal(h.ctx.model.contextWindow, expected);
  }
  const count = h.entries.length;
  for (const argument of ['0', '-1', '1k', '63999', '128k junk', 'Infinity', '1e5', '64.0001k', '132k', 'maximum']) {
    await h.command(argument);
    assert.equal(h.ctx.model.contextWindow, 131072);
    assert.match(h.notices.at(-1)[0], /usage/i);
  }
  assert.equal(h.entries.length, count);
});

test('small-window models still expose their default without inventing a larger capacity', async () => {
  const h = harness({ model: { provider: 'local', id: 'small', contextWindow: 32000 } });
  h.choice = -1;
  await h.command();
  assert.equal(h.menu.options.length, 1);
  await h.command('64k');
  await h.command('16k');
  assert.equal(h.calls.length, 0);
  assert.equal(h.ctx.model.contextWindow, 32000);
});

test('per-model choices are isolated across providers, switching and branch restoration', async () => {
  const a = { provider: 'one', id: 'shared', contextWindow: 500000 };
  const b = { provider: 'two', id: 'shared', contextWindow: 131072 };
  const h = harness({ model: a });
  h.registry.set('two/shared', b);
  await h.command('128k');
  h.ctx.model = b;
  await h.emit('model_select');
  assert.equal(h.ctx.model.contextWindow, 131072);
  await h.command('64k');
  h.ctx.model = a;
  await h.emit('model_select');
  assert.equal(h.ctx.model.contextWindow, 128000);
  const resumed = harness({ model: a, entries: h.entries });
  resumed.registry.set('two/shared', b);
  await resumed.emit('session_start');
  assert.equal(resumed.ctx.model.contextWindow, 128000);
  resumed.ctx.model = b;
  await resumed.emit('model_select');
  assert.equal(resumed.ctx.model.contextWindow, 64000);
  resumed.entries = [];
  await resumed.emit('session_tree');
  assert.equal(resumed.ctx.model.contextWindow, 131072);
});

test('default follows registry changes and invalidated saved budgets are ignored', async () => {
  const h = harness({ model: { provider: 'local', id: 'test', contextWindow: 500000 } });
  await h.command('500k');
  await h.command('default');
  h.registry.set('local/test', { ...h.base, contextWindow: 256000 });
  await h.emit('session_start');
  assert.equal(h.ctx.model.contextWindow, 256000);
  h.entries.push({ type: 'custom', customType: 'context-window', data: {
    provider: 'local', model: 'test', contextWindow: 500000,
  } });
  await h.emit('input');
  assert.equal(h.ctx.model.contextWindow, 256000);
});

test('custom saved budgets appear as current and completions follow the active model', async () => {
  const h = harness();
  await h.emit('session_start');
  assert.ok(h.completions('ex').some(item => item.value === 'extended'));
  const other = { provider: 'local', id: 'test', contextWindow: 131072 };
  h.registry.set('local/test', other);
  h.ctx.model = other;
  await h.emit('model_select');
  assert.equal(h.completions('ex'), null);
  assert.ok(h.completions('def').some(item => item.value === 'default'));
  await h.command('100k');
  h.choice = -1;
  await h.command();
  assert.ok(h.menu.options.some(option => /Custom: 100k.*current/.test(option)));
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
