import assert from 'node:assert/strict';
import { test } from 'node:test';
import { setImmediate } from 'node:timers/promises';
import herdrTab from '../extensions/herdr-tab.ts';

function fixture(initial = 'Pi', lead = 't1') {
  const handlers = new Map();
  const signals = new Map();
  const entries = [];
  const api = {
    on: (event, handler) => handlers.set(event, handler),
    events: { on: (event, handler) => { signals.set(event, handler); return () => signals.delete(event); } },
    getSessionName: () => name,
    appendEntry: (customType, data) => entries.push({ type: 'custom', customType, data }),
  };
  const ctx = { mode: 'tui', cwd: '/work/project', sessionManager: { getBranch: () => entries, getEntries: () => entries } };
  let name;
  const tab = { id: 'local', name: initial, terminals: [{ terminal: lead }, { terminal: 't1' }] };
  const writes = [];
  const app = async (...args: string[]) => {
    if (args[0] === 'ls') return JSON.stringify({ spaces: [
      { machine: 'remote', tabs: [{ ...tab, id: 'remote', terminals: [{ terminal: 't1' }] }] },
      { machine: null, tabs: [tab] },
    ] });
    assert.deepEqual(args.slice(0, 2), ['do', 'tab.rename']);
    const { id, name } = JSON.parse(args[2]);
    assert.equal(id, tab.id);
    writes.push(name);
    tab.name = name;
    return '{"ok":true}';
  };
  const start = async (mode = 'tui') => {
    herdrTab(api, { env: { HC_TERMINAL_ID: 't1' }, app });
    handlers.get('session_start')({}, { ...ctx, mode });
    await setImmediate();
  };
  return { start, tab, writes,
    name: async (value: string) => { name = value; handlers.get('session_info_changed')(); await setImmediate(); },
    summary: async (label: string) => { api.appendEntry('agent-bus-auto-label', { label }); signals.get('herdr:label_changed')(); await setImmediate(); },
    stop: () => handlers.get('session_shutdown')(),
  };
}

test('names new desktop tabs and follows /name without a Switchboard extension', async () => {
  const f = fixture('3');
  await f.start();
  await f.name('Fix parser');
  f.stop();
  await f.start();
  await f.name('Test parser');
  assert.deepEqual(f.writes, ['3 project', '3 Fix parser', '3 Test parser']);
  f.tab.name = 'My manual name';
  await f.name('Do not overwrite');
  assert.equal(f.tab.name, 'My manual name');
});

test('optional generated label can replace the folder but not an explicit name', async () => {
  const f = fixture();
  await f.start();
  await f.summary('Fix consent checks');
  await f.name('Review consent checks');
  await f.summary('Ignored suggestion');
  assert.deepEqual(f.writes, ['project', 'Fix consent checks', 'Review consent checks']);
});

test('headless sessions and secondary panes cannot rename a tab', async () => {
  const rpc = fixture();
  await rpc.start('rpc');
  await rpc.name('Wrong title');
  assert.deepEqual(rpc.writes, []);
  const split = fixture('3', 't2');
  await split.start();
  assert.deepEqual(split.writes, []);
});
