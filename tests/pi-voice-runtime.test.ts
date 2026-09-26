import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, rm, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import net from 'node:net';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { questionModel } from './fixtures/pi-team-question-model.ts';
import { socketCall } from '../home/config/pi/extensions/subagent/herdr.ts';
import { shellQuote } from '../home/config/pi/extensions/subagent/team.ts';

const voiceSource = new URL('../home/config/pi/extensions/pi-voice.ts', import.meta.url);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(predicate, timeout = 20000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const result = await predicate();
    if (result) return result;
    await sleep(50);
  }
  assert.fail('Live voice fixture checkpoint timed out');
}
const shape = node => node.type === 'pane' ? { pane: node.pane_id } : {
  direction: node.direction, ratio: node.ratio, first: shape(node.first), second: shape(node.second),
};

function guardedPanes() {
  assert.equal(process.env.HERDR_ENV, '1');
  assert.ok(process.env.HERDR_SOCKET_PATH && process.env.HERDR_PANE_ID, 'Run inside a real Herdr pane');
  const parent = process.env.HERDR_PANE_ID, owned = new Set();
  let split = false;
  const call = async (method, params) => {
    if (method === 'pane.split') {
      assert.equal(split, false, 'Only one temporary sibling is permitted');
      assert.equal(params.target_pane_id, parent);
      assert.equal(params.focus, false);
      split = true;
    } else if (['pane.send_input', 'pane.close'].includes(method)) {
      assert.ok(owned.has(params.pane_id), `Refusing ${method} on an unowned pane`);
    } else assert.ok(['layout.export', 'pane.current'].includes(method), `Forbidden operation ${method}`);
    const result = await socketCall(process.env.HERDR_SOCKET_PATH, method, params);
    if (method === 'pane.split') {
      assert.ok(result.pane?.pane_id && result.pane.pane_id !== parent);
      owned.add(result.pane.pane_id);
    }
    if (method === 'pane.close') owned.delete(params.pane_id);
    return result;
  };
  return { owned, parent, call, layout: async () => (await call('layout.export', { pane_id: parent })).layout };
}

const controllerFixture = fileURLToPath(new URL('./fixtures/pi-voice-controller.py', import.meta.url));
const controllerCall = (path, request) => new Promise((resolve, reject) => {
  const socket = net.createConnection(path);
  let data = '';
  socket.setTimeout(5000, () => socket.destroy(new Error('Controller request timed out')));
  socket.setEncoding('utf8');
  socket.on('error', reject);
  socket.on('connect', () => socket.end(JSON.stringify(request) + '\n'));
  socket.on('data', chunk => { data += chunk; });
  socket.on('end', () => { try { resolve(JSON.parse(data)); } catch (error) { reject(error); } });
});
async function startController(runtime, extra = []) {
  let output = '', stderr = '';
  const child = spawn('python3', [controllerFixture, '--runtime', runtime, ...extra], {
    env: { PATH: process.env.PATH, HOME: runtime, PYTHONDONTWRITEBYTECODE: '1' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stdout.on('data', chunk => { output += chunk; });
  child.stderr.on('data', chunk => { stderr += chunk; });
  let spawnError;
  child.on('error', error => { spawnError = error; });
  const stop = async () => {
    if (child.exitCode === null && child.signalCode === null) {
      const exited = new Promise(resolve => child.once('exit', resolve));
      child.kill('SIGTERM');
      await exited;
    }
    await rm(join(runtime, 'control.sock'), { force: true });
  };
  try {
    await until(() => {
      if (spawnError) throw spawnError;
      assert.equal(child.exitCode, null, stderr);
      return output.includes('ready\n');
    });
  } catch (error) { await stop(); throw error; }
  return { stop, diagnostics: () => stderr };
}

test('real controller fixture: offline dispatch and restore without audio', async t => {
  const runtime = await mkdtemp(join(tmpdir(), 'piv-offline-'));
  t.after(() => rm(runtime, { recursive: true, force: true }));
  let child = await startController(runtime);
  t.after(() => child.stop());
  const call = request => controllerCall(join(runtime, 'control.sock'), request);
  assert.equal((await call({ action: 'status' })).connection_state, 'unselected');
  const rejected = await call({ action: 'attach', target: {} });
  assert.equal(rejected.ok, true, JSON.stringify(rejected));
  assert.deepEqual(rejected.attached, { state: 'connecting' });
  assert.equal((await call({ action: 'harness-event', token: 'foreign', event: {} })).accepted, false);
  await call({ action: 'team-toggle' });
  await child.stop();
  child = await startController(runtime);
  assert.equal((await call({ action: 'status' })).show_team, true);
  const state = await call({ action: 'fixture-state' });
  assert.deepEqual(state.audio_calls, []);
  assert.deepEqual(state.errors, []);
  assert.deepEqual(state.reads, []);
});

// Live panes and model calls require explicit opt-in; offline tests use only an isolated socket.
test('live Pi voice: startup, reload, new session, question guard and controller restart', {
  skip: process.env.PI_VOICE_LIVE_TEST !== '1' && 'Set PI_VOICE_LIVE_TEST=1 inside Herdr to opt in',
  timeout: 120000,
}, async t => {
  const guard = guardedPanes();
  const before = await guard.layout();
  const focused = (await guard.call('pane.current', {})).pane.pane_id;
  const dir = await mkdtemp(join(tmpdir(), 'piv-live-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const home = join(dir, 'home'), agent = join(home, '.pi', 'agent'), cwd = join(dir, 'project');
  const runtime = join(dir, 'run'), controlDir = join(runtime, 'pi-voice');
  await mkdir(join(agent, 'extensions'), { recursive: true });
  await mkdir(cwd);
  await mkdir(runtime, { mode: 0o700 });
  await mkdir(controlDir, { mode: 0o700 });
  const controllerPath = join(controlDir, 'control.sock');
  const trace = join(dir, 'lifecycle.jsonl'), checkpoint = join(dir, 'checkpoint.json');
  const model = await questionModel();
  t.after(() => model.close());
  await writeFile(join(agent, 'models.json'), JSON.stringify(model.models));
  await writeFile(join(agent, 'settings.json'), JSON.stringify({
    defaultProvider: 'team-question', defaultModel: 'question', defaultProjectTrust: 'no',
    retry: { enabled: false }, compaction: { enabled: false },
  }));
  await writeFile(join(agent, 'extensions', 'pi-voice.ts'), await readFile(voiceSource));
  // Observe shutdown before the bridge so a stalled detach cannot hide Pi's quit event.
  await writeFile(join(agent, 'extensions', 'aa-probe.ts'), `
    import { appendFileSync, writeFileSync } from 'node:fs';
    export default function(pi) {
      let timer;
      const record = (kind, ctx, fields = {}) => appendFileSync(${JSON.stringify(trace)}, JSON.stringify({
        kind, session: ctx.sessionManager.getSessionId(), editor: ctx.ui.getEditorText(),
        idle: ctx.isIdle(), pending: ctx.hasPendingMessages(), ...fields,
      }) + '\\n');
      pi.on('session_start', (_event, ctx) => {
        let previous;
        timer = setInterval(() => {
          const state = JSON.stringify({ editor: ctx.ui.getEditorText(), idle: ctx.isIdle(), pending: ctx.hasPendingMessages() });
          if (state !== previous) { previous = state; record('control_state', ctx); }
        }, 100);
        timer.unref();
      });
      pi.on('session_shutdown', () => clearInterval(timer));
      pi.on('input', (event, ctx) => record('input', ctx, { text: event.text, source: event.source }));
      for (const kind of ['session_start', 'session_shutdown', 'ui_prompt_start', 'ui_prompt_end', 'agent_settled']) {
        pi.on(kind, (event, ctx) => appendFileSync(${JSON.stringify(trace)}, JSON.stringify({
          kind, reason: event.reason, mode: ctx.mode, session: ctx.sessionManager.getSessionId(),
          pid: process.pid, activation: globalThis[Symbol.for('pi.voice.activation')],
        }) + '\\n'));
      }
      pi.registerCommand('voice-checkpoint', { handler: (_args, ctx) => {
        writeFileSync(${JSON.stringify(checkpoint)}, JSON.stringify({ mode: ctx.mode }));
      }});
      pi.registerCommand('voice-question', { handler: async (_args, ctx) => {
        await ctx.ui.select('Owned voice fixture question', ['Cancel'], { timeout: 15000 });
      }});
      pi.registerCommand('voice-exit', { handler: (_args, ctx) => {
        record('voice_exit', ctx);
        ctx.shutdown();
      }});
    }
  `);
  let rows = [], errors = [], fixtureState, child, temporary, previousRows = [];
  const registrations = new Map();
  const refresh = async () => {
    fixtureState = await controllerCall(controllerPath, { action: 'fixture-state' });
    rows = [...previousRows, ...fixtureState.rows];
    errors = fixtureState.errors;
    assert.deepEqual(fixtureState.audio_calls, [], 'Audio must never capture or speak');
    registrations.clear();
    for (const entry of fixtureState.sessions) registrations.set(entry.target.bridge_id, entry);
  };
  const wait = async predicate => {
    try { return await until(async () => { await refresh(); return predicate(); }); }
    catch (error) {
      t.diagnostic(`Controller checkpoint ${predicate}: ${JSON.stringify({
        sessions: fixtureState?.sessions, errors, events: rows.map(row => row.action === 'harness-event' ? row.event.type : row.action),
        reads: fixtureState?.reads.map(read => read.command),
      })}`);
      throw error;
    }
  };
  const openController = async (hold = false) => {
    child = await startController(controlDir, ['--pane', temporary,
      '--herdr-socket', process.env.HERDR_SOCKET_PATH, ...(hold ? ['--hold-first'] : [])]);
  };
  const closeController = async () => { if (child) { await child.stop(); child = null; } };
  t.after(closeController);
  const readTrace = async () => {
    try { return (await readFile(trace, 'utf8')).trim().split('\n').filter(Boolean).map(JSON.parse); }
    catch (error) { if (error.code === 'ENOENT') return []; throw error; }
  };
  const bridgeRequest = (entry, command, fields = {}) => new Promise((resolve, reject) => {
    assert.ok(entry.target.adapter_socket.startsWith(controlDir + '/pi-'));
    const socket = net.createConnection(entry.target.adapter_socket);
    let data = '';
    const timer = setTimeout(() => socket.destroy(new Error('Owned bridge timed out')), 2000);
    socket.setEncoding('utf8');
    socket.on('error', reject);
    socket.on('close', () => clearTimeout(timer));
    socket.on('connect', () => socket.write(JSON.stringify({ ...entry.target, token: entry.token, command, ...fields }) + '\n'));
    socket.on('data', chunk => { data += chunk; });
    socket.on('end', () => { try { resolve(JSON.parse(data)); } catch (error) { reject(error); } });
  });
  // Preserve the checkpoint failure even if external focus changes also fail cleanup checks.
  let failure;
  const command = text => guard.call('pane.send_input', { pane_id: temporary, text, keys: ['Enter'] });
  try {
    temporary = (await guard.call('pane.split', { target_pane_id: guard.parent,
      direction: 'right', ratio: 0.5, focus: false, cwd, env: {} })).pane.pane_id;
    await openController(true);
    // env -i and a loopback-only model catalogue isolate credentials, extension discovery,
    // controller endpoint, configuration and sessions from the user's live Pi environment.
    const env = { PATH: process.env.PATH, HOME: home, XDG_CONFIG_HOME: join(home, '.config'),
      XDG_RUNTIME_DIR: runtime, PI_CODING_AGENT_DIR: agent, TMPDIR: dir,
      PI_OFFLINE: '1', PI_TELEMETRY: '0', CAPTURE_PROMPTS: '0', TERM: 'xterm-256color', LANG: 'C.UTF-8',
      HERDR_ENV: '1', HERDR_SOCKET_PATH: process.env.HERDR_SOCKET_PATH, HERDR_PANE_ID: temporary,
      AGENT_VOICE_SOCKET: controllerPath };
    const args = [process.env.PI_VOICE_TEST_BIN ?? 'pi', '--offline', '--no-skills', '--no-prompt-templates',
      '--no-context-files', '--no-builtin-tools', '--model', 'team-question/question', '--thinking', 'off',
      '--session-dir', join(dir, 'sessions'), '/voice-checkpoint'];
    await command(`exec env -i ${Object.entries(env).map(([key, value]) => shellQuote(`${key}=${value}`)).join(' ')} ${args.map(shellQuote).join(' ')}`);
    await until(async () => {
      try { return JSON.parse(await readFile(checkpoint, 'utf8')).mode === 'tui'; }
      catch (error) { if (error.code === 'ENOENT') return false; throw error; }
    });
    const first = await wait(() => rows.find(row => row.action === 'attach')?.target);
    assert.equal(rows.some(row => row.event?.type === 'ready'), false, 'Pi startup finished while attachment remained unresolved');
    await command('/reload');
    const second = await wait(() => [...registrations.values()].find(entry => entry.ready && entry.target.bridge_id !== first.bridge_id));
    assert.equal(second.target.managed, true);
    assert.ok(second.target.start);
    assert.equal(second.target.adapter_socket, join(controlDir, `pi-${second.target.pid}-${second.target.bridge_id}.sock`));
    assert.equal((await stat(second.target.adapter_socket)).mode & 0o777, 0o600);
    assert.equal(second.target.team_child, false);
    const evidence = fixtureState.reads.find(read => read.command[0] === 'pane');
    assert.ok(evidence.result.process_info.foreground_processes.some(proc => proc.pid === second.target.pid));
    assert.equal(second.target.pid, first.pid);
    assert.equal(second.target.session, first.session);
    assert.ok(second.target.activation > first.activation, 'Actual Pi reload shares the process-global ordinal');
    await controllerCall(controllerPath, { action: 'fixture-release' });
    await sleep(150);
    await refresh();
    assert.equal(rows.some(row => row.event?.type === 'ready' && row.event.bridge_id === first.bridge_id), false);
    assert.equal((await bridgeRequest(second, 'status')).result.activation, second.target.activation);
    const afterReload = await readTrace();
    assert.ok(afterReload.some(row => row.kind === 'session_shutdown' && row.reason === 'reload'));
    assert.ok(afterReload.some(row => row.kind === 'session_start' && row.reason === 'reload'));

    // EOF from ordinary status/stage calls never detaches a live bridge.
    await bridgeRequest(second, 'status');
    await bridgeRequest(second, 'stage', { text: 'loopback voice fixture' });
    assert.equal(registrations.get(second.target.bridge_id)?.ready, true);
    assert.equal((await bridgeRequest(second, 'submit')).ok, true);
    await wait(() => rows.some(row => row.event?.type === 'reply'));
    assert.equal(model.histories.length, 1, 'Only the explicitly submitted loopback prompt reaches a model');
    await command('/voice-question');
    await until(async () => (await bridgeRequest(second, 'status')).result.state === 'blocked');
    assert.equal((await bridgeRequest(second, 'stage', { text: 'must not answer dialog' })).ok, false);
    await guard.call('pane.send_input', { pane_id: temporary, keys: ['Escape'] });
    await until(async () => (await bridgeRequest(second, 'status')).result.ready);

    await bridgeRequest(second, 'stage', { text: '/new' });
    // Press Enter on the staged built-in command in this owned pane. This exercises
    // real session replacement without appending a command to an unrelated draft.
    await command('');
    const third = await wait(() => [...registrations.values()].find(entry => entry.ready && entry.target.session !== second.target.session));
    assert.equal(third.target.pid, second.target.pid);
    assert.ok(third.target.activation > second.target.activation);
    assert.equal((await bridgeRequest(third, 'submit')).ok, false);
    assert.equal((await bridgeRequest(third, 'stage', { text: 'stale', bridge_id: second.target.bridge_id,
      activation: second.target.activation, session: second.target.session })).ok, false);
    assert.ok(rows.some(row => row.action === 'detach' && row.bridge_id === second.target.bridge_id));

    const heartbeat = third.heartbeat_at;
    await wait(() => registrations.get(third.target.bridge_id)?.heartbeat_at > heartbeat &&
      rows.some(row => row.event?.type === 'heartbeat' && row.event.bridge_id === third.target.bridge_id));
    const controllerStatus = await controllerCall(controllerPath, { action: 'status' });
    assert.equal(controllerStatus.pane, temporary);
    assert.equal(controllerStatus.connection_state, 'ready');
    assert.equal(controllerStatus.harness, 'pi');
    const readyCount = rows.filter(row => row.event?.type === 'ready').length;
    assert.deepEqual(errors, []);
    await closeController();
    previousRows = rows;
    registrations.clear();
    await openController();
    await refresh();
    assert.ok(fixtureState.restored.some(entry => entry.token === third.token && entry.state === 'connecting'));
    assert.equal((await controllerCall(controllerPath, { action: 'status' })).ok, true);
    const restored = await wait(() => registrations.get(third.target.bridge_id)?.ready && registrations.get(third.target.bridge_id));
    assert.equal(restored.token, third.token, 'Real restore retains the validated admission token');
    assert.equal(restored.target.activation, third.target.activation);
    assert.ok(rows.filter(row => row.event?.type === 'ready').length > readyCount);
    assert.equal((await bridgeRequest(restored, 'status')).ok, true);
    assert.equal((await bridgeRequest(restored, 'submit')).ok, false);
    assert.equal(model.histories.length, 1, 'Reconnect must never replay submission');
    assert.deepEqual(model.errors, []);
    await command('/voice-exit');
    await wait(() => rows.some(row => row.action === 'detach' && row.bridge_id === third.target.bridge_id));
    await until(async () => (await readTrace()).some(row => row.kind === 'session_shutdown' && row.reason === 'quit'));
    assert.deepEqual(errors, []);
    assert.equal((await guard.layout()).focused_pane_id, before.focused_pane_id);
    assert.equal((await guard.call('pane.current', {})).pane.pane_id, focused);
  } catch (error) {
    failure = error;
    t.diagnostic(`Controller stderr: ${child?.diagnostics()}`);
    t.diagnostic(`Live failure before cleanup: ${error.stack}`);
    try { t.diagnostic(`Pi lifecycle: ${JSON.stringify(await readTrace())}`); }
    catch (diagnosticError) { t.diagnostic(`Lifecycle read failed: ${diagnosticError.message}`); }
  } finally {
    const cleanupErrors = [];
    for (const pane_id of [...guard.owned]) {
      try { await guard.call('pane.close', { pane_id }); }
      catch (error) { if (error.code !== 'pane_not_found') cleanupErrors.push(error); }
    }
    try { await closeController(); } catch (error) { cleanupErrors.push(error); }
    try {
      const after = await guard.layout();
      assert.deepEqual(shape(after.root), shape(before.root));
      assert.equal(after.focused_pane_id, before.focused_pane_id);
      assert.equal(after.tab_id, before.tab_id);
      assert.equal(after.workspace_id, before.workspace_id);
      assert.equal((await guard.call('pane.current', {})).pane.pane_id, focused);
    } catch (error) { cleanupErrors.push(error); }
    for (const error of cleanupErrors) t.diagnostic(`Cleanup failure: ${error.stack}`);
    failure ??= cleanupErrors[0];
  }
  if (failure) throw failure;
});
