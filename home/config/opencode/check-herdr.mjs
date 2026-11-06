// Targeted, offline regressions for the desktop migration. Run with Node 24+.
import assert from 'node:assert/strict';
import { nameUnlabelledTab, herdrTarget } from './harness/herdr-tab.ts';
import { createDesktopMetadata, modelLabel } from './harness/herdr-metadata.ts';
import herdrModel from '../pi/extensions/herdr-model.ts';

const env = { HC_TERMINAL_ID: 't8', HERDR_ENV: '1', HERDR_SOCKET_PATH: '/stale.sock', HERDR_PANE_ID: 'old-pane' };
assert.equal(herdrTarget(env), 'herdr-app:default:t8');
assert.equal(herdrTarget({}), undefined);
assert.equal(herdrTarget({ HERDR_ENV: '1', HERDR_SOCKET_PATH: '/old.sock', HERDR_PANE_ID: 'p2' }), '/old.sock:p2');
let tabName = '2';
let terminals = ['t8', 't9'];
let reads = 0;
let onRead;
const writes = [];
const appCall = async args => {
  if (args[0] === 'ls') {
    onRead?.(++reads);
    return JSON.stringify({ spaces: [{ tabs: [{ id: 'own-tab', name: tabName, terminals: terminals.map(terminal => ({ terminal })) }] }] });
  }
  assert.deepEqual(args.slice(0, 2), ['do', 'tab.rename']);
  const value = JSON.parse(args[2]);
  assert.equal(value.id, 'own-tab');
  tabName = value.name;
  writes.push(value);
  return '{}';
};
const noLegacyCall = () => { throw new Error('Desktop must not use the stale legacy socket'); };
let previous = await nameUnlabelledTab('Desktop session', env, noLegacyCall, { appCall });
assert.equal(tabName, '2 Desktop session');
previous = await nameUnlabelledTab('Updated title', env, noLegacyCall, { appCall, previous });
assert.equal(tabName, '2 Updated title');
tabName = 'Manual name';
await nameUnlabelledTab('Do not overwrite', env, noLegacyCall, { appCall, previous });
assert.equal(tabName, 'Manual name');
tabName = '2';
terminals = ['t9', 't8'];
await nameUnlabelledTab('Secondary pane', env, noLegacyCall, { appCall });
assert.equal(tabName, '2');
terminals = ['t8'];
reads = 0;
onRead = count => { if (count === 2) tabName = 'Manual during lookup'; };
await nameUnlabelledTab('Racing title', env, noLegacyCall, { appCall });
assert.equal(tabName, 'Manual during lookup');
onRead = undefined;
tabName = '2';
await nameUnlabelledTab('Cancelled', env, noLegacyCall, { appCall, isCurrent: () => false });
await nameUnlabelledTab('Invalid\x1btitle', env, noLegacyCall, { appCall });
assert.equal(tabName, '2');
assert.equal(writes.length, 2);

let legacyName = '3';
const legacyCall = async (method, params) => {
  if (method === 'pane.get') return { pane: { pane_id: 'w1:p2', tab_id: 'w1:t1', workspace_id: 'w1' } };
  if (method === 'pane.list') return { panes: ['w1:p10', 'w1:p2'].map(pane_id => ({ pane_id, tab_id: 'w1:t1' })) };
  if (method === 'tab.get') return { tab: { label: legacyName } };
  if (method === 'tab.rename') legacyName = params.label;
};
await nameUnlabelledTab('Legacy session', { HERDR_ENV: '1', HERDR_SOCKET_PATH: '/old.sock', HERDR_PANE_ID: 'w1:p2' }, legacyCall);
assert.equal(legacyName, '3 Legacy session');

assert.equal(modelLabel({ modelID: 'gpt-test', variant: 'high' }), 'opencode · high · gpt-test');
assert.equal(modelLabel(undefined), '');
const labels = [];
let release;
const metadata = createDesktopMetadata(env, async label => {
  labels.push(label);
  if (labels.length === 1) await new Promise(resolve => { release = resolve; });
});
metadata.sync({ modelID: 'first', variant: 'high' });
metadata.sync({ modelID: 'first', variant: 'high' });
metadata.sync({ modelID: 'latest', variant: 'low' });
metadata.stop();
release();
await new Promise(resolve => setImmediate(resolve));
assert.deepEqual(labels, ['opencode · high · first', '']);
metadata.sync({ modelID: 'stopped' });
await new Promise(resolve => setImmediate(resolve));
assert.equal(labels.length, 2);
let attempts = 0;
const retry = createDesktopMetadata(env, async () => { if (++attempts === 1) throw new Error('core unavailable'); });
retry.sync({ modelID: 'retry' });
await new Promise(resolve => setImmediate(resolve));
retry.sync({ modelID: 'retry' });
await new Promise(resolve => setImmediate(resolve));
assert.equal(attempts, 2);
const outside = createDesktopMetadata({}, async () => { throw new Error('outside Herdr'); });
outside.sync({ modelID: 'none' });

const events = new Map();
const piReports = [];
let heartbeat;
let cancelled = false;
herdrModel({ on: (name, callback) => events.set(name, callback) }, {
  env: { HC_TERMINAL_ID: 't8' },
  send: async report => piReports.push(report),
  every: callback => { heartbeat = callback; return { unref() {} }; },
  cancel: () => { cancelled = true; },
});
await events.get('session_start')({}, { mode: 'rpc', model: { id: 'headless' } });
assert.equal(piReports.length, 0);
await events.get('session_start')({}, { mode: 'tui', thinkingLevel: 'high', model: { id: 'pi-model' } });
assert.equal(piReports.at(-1).display_agent, 'pi · high · pi-model');
await events.get('model_select')({}, { thinkingLevel: 'low', model: { id: 'other-model' } });
assert.equal(piReports.at(-1).display_agent, 'pi · low · other-model');
await heartbeat();
await events.get('session_shutdown')();
assert.equal(piReports.at(-1).clear_display_agent, true);
assert.equal(cancelled, true);
console.log('PASS: desktop ownership, stale legacy variables, manual rename, pane election, cancellation, legacy compatibility, model updates/cleanup/retry, and Pi TUI-only metadata');
