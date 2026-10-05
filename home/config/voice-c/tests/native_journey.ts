// Real daemon, synthetic PCM, loopback transcription and a managed Pi fixture.
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import http from 'node:http';
import { join, resolve } from 'node:path';
import { cleanup, delay, environment, listen, request, start, tool, until, wait } from './support.ts';

const binary = resolve(process.argv[2]);
const root = await mkdtemp('/tmp/vj-'); // Short enough for sockaddr_un, even in Nix.
const bodies: Buffer[] = [];
let child, adapter, herdr;
const speech = http.createServer(async (req, res) => {
  try {
    let payload = { status: 'ok' };
    if (req.method === 'POST') {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      bodies.push(Buffer.concat(chunks));
      const number = bodies.length;
      if (number === 1) await delay(4200); // Accumulate three queued speech slices.
      payload = { text: `Native phrase ${number}.` };
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(payload));
  } catch (error) { res.destroy(error); }
});
try {
  await new Promise<void>(resolve => speech.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${speech.address().port}`;
  const env = await environment(root, {
    stt_url: base + '/inference', stt_health_url: base + '/health',
    tts_url: base + '/speech', tts_health_url: base + '/models',
  });
  const privateDir = join(env.XDG_RUNTIME_DIR, 'pi-voice');
  await mkdir(privateDir, { mode: 0o700 });
  const pid = process.pid, bridge = randomUUID(), pane = 'w1:p1', session = 'native-fixture-session';
  const adapterPath = join(privateDir, `pi-${pid}-${bridge}.sock`);
  const herdrPath = join(root, 'herdr.sock'), control = join(privateDir, 'control.sock');
  const target = {
    pane, socket: herdrPath, pid, session, bridge_id: bridge, activation: 1,
    adapter_socket: adapterPath, harness: 'pi', team_child: false,
    model: 'synthetic-model', thinking: 'medium',
  };
  const identity = Object.fromEntries(['pid', 'session', 'bridge_id', 'activation', 'harness'].map(key => [key, target[key]]));
  const state = { token: null, staged: [], submitted: [], armed: false, text: '' };
  adapter = await listen(adapterPath, command => {
    const valid = command.token === state.token && Object.entries(identity).every(([key, value]) => command[key] === value);
    if (!valid) return { ok: false, error: 'fixture identity mismatch' };
    if (command.command === 'stage') {
      state.text += (state.text ? '\n' : '') + command.text;
      state.staged.push(command.text);
      state.armed = true;
      return { ok: true, result: {} };
    }
    if (command.command === 'submit' && state.armed) {
      state.submitted.push(state.text);
      state.armed = false;
      return { ok: true, result: {} };
    }
    if (command.command === 'status') return { ok: true, result: {
      ...identity, ready: true, accepts_input: true, state: 'idle',
      draft: state.armed, draft_state: state.armed ? 'staged' : 'none',
    } };
    return { ok: false, error: 'no owned draft' };
  });
  herdr = await listen(herdrPath, () => ({ ok: true }));
  const dump = [
    { type: 'PipeWire:Interface:Node', info: {
      props: { 'node.name': 'synthetic', 'node.description': 'Synthetic microphone', 'media.class': 'Audio/Source' },
      params: { Props: [{ mute: false }] },
    } },
    { props: { 'metadata.name': 'default' }, metadata: [{ key: 'default.audio.source', value: { name: 'synthetic' } }] },
  ];
  const herdrResult = { ok: true, result: {
    process_info: { pane_id: pane, foreground_processes: [{ pid, name: 'pi' }] },
    snapshot: {
      workspaces: [{ workspace_id: 'w1', name: 'Native fixture' }],
      tabs: [{ tab_id: 't1', name: 'Synthetic pane' }],
      panes: [{ pane_id: pane, workspace_id: 'w1', tab_id: 't1' }],
    },
  } };
  const source = `const fs = require('node:fs'), path = require('node:path');
fs.appendFileSync(${JSON.stringify(join(root, 'calls.jsonl'))}, JSON.stringify(process.argv.slice(1)) + '\\n');
const name = path.basename(process.argv[1]);
(async () => {
if (name === 'systemctl') { if (process.argv.includes('show')) console.log('inactive'); }
else if (name === 'pw-dump') console.log(${JSON.stringify(JSON.stringify(dump))});
else if (name === 'herdr') console.log(${JSON.stringify(JSON.stringify(herdrResult))});
else if (name === 'pw-record') {
  process.on('SIGINT', () => process.exit(0));
  process.on('SIGTERM', () => process.exit(0));
  for (let frame = 0; frame < 500; frame++) {
    const voiced = [0, 60, 120, 180].some(start => start <= frame && frame < start + 20);
    const pcm = Buffer.alloc(640);
    for (let i = 0; i < 320; i++) pcm.writeInt16LE(voiced ? Math.trunc(1500 * Math.sin(i * 0.17)) : 0, i * 2);
    try { fs.writeSync(1, pcm); } catch (error) { if (error.code === 'EPIPE') break; throw error; }
    await new Promise(resolve => setTimeout(resolve, 20));
  }
} else if (name === 'pw-play') { for await (const _ of process.stdin) {} }
else process.exit(97);
})().catch(error => { console.error(error); process.exitCode = 1; });
`;
  for (const name of ['systemctl', 'pw-dump', 'pw-record', 'pw-play', 'herdr', 'wl-copy']) {
    await tool(join(env.PATH, name), source);
  }
  child = start(binary, ['serve'], env);
  await until(async () => {
    assert.equal(child.output.done, false, child.output.stderr);
    try { return (await request(control, { action: 'status' })).ok; }
    catch { return false; }
  }, 'controller startup');
  const attached = await request(control, { action: 'attach', target });
  assert.equal(attached.ok, true);
  assert.ok(attached.attached.token);
  state.token = attached.attached.token;
  const event = { ...identity, type: 'ready', ready: true, accepts_input: true, state: 'idle', adapter_socket: adapterPath, draft_state: 'none' };
  assert.equal((await request(control, { action: 'harness-event', token: state.token, event })).accepted, true);
  const status = await request(control, { action: 'status' });
  assert.equal(status.pane, pane);
  assert.equal(status.connection_state, 'ready');
  assert.equal((await request(control, { action: 'harness-event', token: state.token, event: { ...event, session: 'foreign' } })).accepted, false);
  assert.equal((await request(control, { action: 'record' })).ok, true);
  await until(() => state.staged.length >= 4, 'four queued slices through slow STT', 20000);
  assert.deepEqual(state.submitted, [], 'Recording submitted without explicit Send');
  assert.equal((await request(control, { action: 'interact' })).ok, true);
  await until(async () => (await request(control, { action: 'status' })).phase === 'draft', 'recording stopped');
  const expected = Array.from({ length: 4 }, (_, i) => `Native phrase ${i + 1}.`);
  assert.equal(bodies.length, 4);
  assert.ok(bodies.every(body => body.includes(Buffer.from('RIFF'))));
  assert.deepEqual(state.staged, expected);
  const microphone = (await request(control, { action: 'status' })).microphone;
  assert.equal(microphone.name, 'Synthetic microphone');
  assert.equal(microphone.target, null);
  assert.equal(microphone.muted, false);
  assert.equal(microphone.missing, false);
  assert.equal((await request(control, { action: 'send' })).ok, true);
  assert.deepEqual(state.submitted, [expected.join('\n')]);
  await request(control, { action: 'send' });
  assert.equal(state.submitted.length, 1, 'Duplicate Send replayed the draft');
  assert.equal((await request(control, { action: 'detach', token: state.token, ...identity })).accepted, true);
  assert.deepEqual((await request(control, { action: 'status' })).sessions, []);
  child.child.kill('SIGTERM');
  const result = await wait(child, 8000);
  assert.equal(result.code, 0, result.stderr);
  console.log('Journey: identity guards, four queued PCM slices, STT, staged draft, explicit Send, detach and shutdown');
} catch (error) {
  if (child) console.error(child.output.stderr);
  throw error;
} finally {
  await cleanup(child);
  if (adapter) await adapter.close();
  if (herdr) await herdr.close();
  speech.closeAllConnections();
  await new Promise<void>(resolve => speech.close(() => resolve()));
  await rm(root, { recursive: true, force: true });
}
