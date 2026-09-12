import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import extension, { displayLabel, sendMetadata } from '../home/config/pi/extensions/herdr-model.ts';

const env = { HERDR_ENV: '1', HERDR_SOCKET_PATH: '/fake', HERDR_PANE_ID: 'w1:p1' };
const ctx = { mode: 'tui', model: { id: 'gpt-test' }, thinkingLevel: 'high' };

function harness(overrides = {}) {
  const handlers = new Map();
  const reports = [];
  let heartbeat;
  let cancelled = 0;
  extension({ on: (name, fn) => handlers.set(name, fn) }, {
    env: { ...env, ...overrides },
    send: async (params) => { reports.push(params); },
    every: (fn, ms) => { assert.equal(ms, 5000); heartbeat = fn; return { unref() {} }; },
    cancel: () => { cancelled++; heartbeat = undefined; },
  });
  return { handlers, reports, tick: () => heartbeat?.(), cancelled: () => cancelled };
}

test('label preserves effort before long models and handles unknowns', () => {
  assert.equal(displayLabel(ctx), 'pi · high · gpt-test');
  assert.equal(displayLabel({}), 'pi · ? · model unknown');
  const label = displayLabel({ model: { id: '界'.repeat(100) }, thinkingLevel: 'off' });
  assert.equal([...label].length, 80);
  assert.ok(label.startsWith('pi · off · '));
  assert.ok(label.endsWith('…'));
  assert.equal(displayLabel({ model: { id: '\nfoo\u001b\u007f' } }), 'pi · ? · foo');
});

test('team labels use the role only for children and retain safe fallbacks', () => {
  assert.equal(displayLabel(ctx, { PI_TEAM_CHILD: '1', PI_TEAM_ROLE: 'scout' }), 'pi · scout · gpt-test');
  assert.equal(displayLabel(ctx, { PI_TEAM_ROLE: 'scout' }), 'pi · high · gpt-test');
  for (const role of [undefined, '', ' \n\u001b']) {
    assert.equal(displayLabel(ctx, { PI_TEAM_CHILD: '1', PI_TEAM_ROLE: role }), 'pi · high · gpt-test');
  }
  assert.equal(displayLabel(ctx, { PI_TEAM_CHILD: '1', PI_TEAM_ROLE: '\nscout\u001b' }), 'pi · scout · gpt-test');
  const label = displayLabel({ ...ctx, model: { id: '界'.repeat(100) } }, {
    PI_TEAM_CHILD: '1', PI_TEAM_ROLE: 'security-reviewer',
  });
  assert.equal([...label].length, 80);
  assert.ok(label.startsWith('pi · security-reviewer · '));
  assert.ok(label.endsWith('…'));
});

test('team role survives model and effort changes, heartbeats and reloads', async () => {
  const h = harness({ PI_TEAM_CHILD: '1', PI_TEAM_ROLE: 'scout' });
  await h.handlers.get('session_start')({}, ctx);
  assert.equal(h.reports.at(-1).display_agent, 'pi · scout · gpt-test');
  const changed = { ...ctx, model: { id: 'other' }, thinkingLevel: 'low' };
  await h.handlers.get('model_select')({}, changed);
  await h.handlers.get('thinking_level_select')({}, changed);
  await h.tick();
  assert.equal(h.reports.at(-1).display_agent, 'pi · scout · other');
  await h.handlers.get('session_shutdown')({ reason: 'reload' });
  assert.equal(h.reports.at(-1).clear_display_agent, true);
  await h.handlers.get('session_start')({ reason: 'reload' }, changed);
  assert.equal(h.reports.at(-1).display_agent, 'pi · scout · other');
});

test('resources start only in TUI sessions and events report live selections', async () => {
  const h = harness();
  assert.equal(h.reports.length, 0);
  await h.handlers.get('session_start')({}, { ...ctx, mode: 'rpc', hasUI: true });
  await h.handlers.get('model_select')({}, ctx);
  assert.equal(h.reports.length, 0);
  await h.handlers.get('session_start')({}, ctx);
  assert.equal(h.reports.at(-1).display_agent, 'pi · high · gpt-test');
  assert.equal(h.reports.at(-1).agent, 'pi');
  assert.equal(h.reports.at(-1).ttl_ms, 15000);
  assert.equal(h.reports.at(-1).source, 'user:pi-model');
  assert.equal(h.reports.at(-1).state, undefined);
  await h.handlers.get('model_select')({}, { ...ctx, model: { id: 'other' } });
  await h.handlers.get('thinking_level_select')({}, { ...ctx, model: { id: 'other' }, thinkingLevel: 'xhigh' });
  assert.equal(h.reports.at(-1).display_agent, 'pi · xhigh · other');
  await h.tick();
  assert.equal(h.reports.at(-1).display_agent, 'pi · xhigh · other');
  assert.ok(h.reports.every((r, i) => i === 0 || r.seq > h.reports[i - 1].seq));
  await h.handlers.get('session_shutdown')({ reason: 'reload' });
  assert.equal(h.cancelled(), 1);
  assert.equal(h.reports.at(-1).clear_display_agent, true);
  const count = h.reports.length;
  await h.tick();
  await h.handlers.get('model_select')({}, ctx);
  await h.handlers.get('session_shutdown')({ reason: 'quit' });
  assert.equal(h.reports.length, count);
});

test('a failed cosmetic report never fails the session', async () => {
  const handlers = new Map();
  extension({ on: (name, fn) => handlers.set(name, fn) }, {
    env,
    send: async () => { throw new Error('server stopped'); },
    every: () => ({ unref() {} }),
    cancel: () => {},
  });
  await handlers.get('session_start')({}, ctx);
  await handlers.get('thinking_level_select')({}, ctx);
  await handlers.get('session_shutdown')({ reason: 'quit' });
});

test('disabled outside herdr', () => {
  extension({ on() { assert.fail('must not register handlers'); } }, { env: {} });
});

test('a server that never answers cannot hang the session', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'herdr-model-'));
  const socket = join(dir, 's');
  const clients = new Set();
  const server = createServer((client) => { clients.add(client); client.resume(); });
  await new Promise((resolve) => server.listen(socket, resolve));
  try {
    await sendMetadata({}, { ...env, HERDR_SOCKET_PATH: socket });
  } finally {
    for (const client of clients) client.destroy();
    await new Promise((resolve) => server.close(resolve));
    await rm(dir, { recursive: true, force: true });
  }
});

test('transport sends only metadata and handles fragmented replies', async () => {
  const dir = await mkdtemp(join(tmpdir(), 'herdr-model-'));
  const socket = join(dir, 's');
  const requests = [];
  const server = createServer((client) => {
    let input = '';
    client.on('data', (chunk) => {
      input += chunk;
      if (!input.includes('\n')) return;
      requests.push(JSON.parse(input));
      client.write('{"result":');
      client.end('{}}\n');
    });
  });
  await new Promise((resolve) => server.listen(socket, resolve));
  try {
    await sendMetadata({ display_agent: 'pi · high · test' }, { ...env, HERDR_SOCKET_PATH: socket });
    assert.equal(requests[0].method, 'pane.report_metadata');
    assert.equal(requests[0].params.pane_id, env.HERDR_PANE_ID);
    assert.equal(requests[0].params.display_agent, 'pi · high · test');
    await sendMetadata({}, { ...env, HERDR_SOCKET_PATH: join(dir, 'missing') });
  } finally {
    await new Promise((resolve) => server.close(resolve));
    await rm(dir, { recursive: true, force: true });
  }
});
