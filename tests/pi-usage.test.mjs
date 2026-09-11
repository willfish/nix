import test from 'node:test';
import assert from 'node:assert/strict';
import install from '../home/config/pi/extensions/usage.js';

function fixture(registry, provider = 'openai-codex') {
  let handler;
  install({ registerCommand(name, command) { assert.equal(name, 'usage'); handler = command.handler; } });
  const notices = [];
  const ctx = { hasUI: true, modelRegistry: registry,
    model: { provider, id: 'test', contextWindow: 100000 }, getContextUsage: () => ({ tokens: 10000 }),
    ui: { notify: (...args) => notices.push(args) } };
  return { ctx, notices, run: () => handler('', ctx) };
}
test('uses coordinated registry token and preserves allowance rendering', async t => {
  let resolutions = 0;
  const f = fixture({ async getProviderAuth(provider) {
    assert.equal(provider, 'openai-codex'); resolutions++; return { auth: { apiKey: 'fixture-token' } };
  } });
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(url, 'https://chatgpt.com/backend-api/wham/usage');
    assert.equal(options.headers.Authorization, 'Bearer fixture-token');
    return { ok: true, json: async () => ({ plan_type: 'pro', rate_limit: { allowed: false,
      primary_window: { used_percent: 75, limit_window_seconds: 18000, reset_after_seconds: 3600 } } }) };
  });
  await f.run();
  assert.equal(resolutions, 1);
  assert.match(f.notices[0][0], /25% remaining, resets in 1h 0m/);
  assert.match(f.notices[0][0], /Session context: 10k\/100k/);
  assert.equal(f.notices[0][1], 'warning');
});
for (const kind of ['missing', 'empty', 'error']) test(`resolver ${kind} fails closed without store fallback or refresh`, async t => {
  t.mock.method(globalThis, 'fetch', async () => assert.fail('No request without resolved credentials'));
  // A path that cannot be read also distinguishes old fallback ENOENT from the intended diagnostic.
  const previous = process.env.PI_CODING_AGENT_DIR;
  process.env.PI_CODING_AGENT_DIR = '/nonexistent/pi-usage-fixture';
  t.after(() => { if (previous === undefined) delete process.env.PI_CODING_AGENT_DIR; else process.env.PI_CODING_AGENT_DIR = previous; });
  const registry = kind === 'missing' ? {} : { async getProviderAuth() {
    if (kind === 'error') throw new Error('sensitive resolver internals');
    return { auth: {} };
  } };
  const f = fixture(registry);
  await f.run();
  assert.match(f.notices[0][0], /run \/login openai-codex/);
  assert.doesNotMatch(f.notices[0][0], /ENOENT|sensitive resolver internals/);
});
test('headless and usage-based providers never resolve credentials or fetch', async t => {
  t.mock.method(globalThis, 'fetch', async () => assert.fail('Unexpected fetch'));
  const f = fixture({ getProviderAuth() { assert.fail('Unexpected auth'); } }, 'other');
  await f.run();
  assert.match(f.notices[0][0], /usage-based provider/);
  f.ctx.hasUI = false;
  await f.run();
  assert.equal(f.notices.length, 1);
});
test('concurrent usage calls await their own coordinated resolutions', async t => {
  const pending = [], tokens = [];
  const f = fixture({ getProviderAuth: () => new Promise(resolve => pending.push(resolve)) });
  t.mock.method(globalThis, 'fetch', async (_url, options) => {
    tokens.push(options.headers.Authorization);
    return { ok: true, json: async () => ({}) };
  });
  const first = f.run(), second = f.run();
  assert.deepEqual(tokens, []);
  pending[1]({ auth: { apiKey: 'newer-fixture' } });
  await second;
  pending[0]({ auth: { apiKey: 'earlier-fixture' } });
  await first;
  assert.deepEqual(tokens, ['Bearer newer-fixture', 'Bearer earlier-fixture']);
  assert.equal(f.notices.length, 2);
});

test('401 does not attempt manual refresh', async t => {
  let calls = 0;
  t.mock.method(globalThis, 'fetch', async () => { calls++; return { status: 401 }; });
  const f = fixture({ getProviderAuth: async () => ({ auth: { apiKey: 'fixture' } }) });
  await f.run();
  assert.equal(calls, 1);
  assert.match(f.notices[0][0], /401.*run \/login/);
});
