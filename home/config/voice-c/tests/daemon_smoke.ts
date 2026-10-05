// Production native daemon with private sockets and fake devices only.
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdtemp, mkdir, readFile, rm, stat, writeFile } from 'node:fs/promises';
import net from 'node:net';
import { join, resolve, basename } from 'node:path';
import { cleanup, environment, listen, request, run, start, tool, until, wait } from './support.ts';

const binary = resolve(process.argv[2]);
const root = await mkdtemp('/tmp/voice-daemon-');
let child, server;
try {
  const env = await environment(root, {
    backends: [{ id: 'fixture', listen_url: 'http://127.0.0.1:9/v1/listen' }],
  });
  const calls = join(root, 'calls.jsonl');
  const source = `const fs = require('node:fs'), path = require('node:path');
fs.appendFileSync(${JSON.stringify(calls)}, JSON.stringify(process.argv.slice(1)) + '\\n');
const name = path.basename(process.argv[1]);
if (name === 'systemctl') { if (process.argv.includes('show')) console.log('inactive'); }
else if (name === 'pw-dump') console.log('[]');
else process.exit(97);
`;
  for (const name of ['systemctl', 'pw-dump', 'pw-record', 'pw-play', 'herdr', 'wl-copy']) {
    await tool(join(env.PATH, name), source);
  }
  const path = join(env.XDG_RUNTIME_DIR, 'pi-voice/control.sock');
  async function launch() {
    const process = start(binary, ['serve'], env);
    try {
      await until(async () => {
        assert.equal(process.output.done, false, process.output.stderr);
        try { return (await request(path, { action: 'status' }, 500)).ok; }
        catch { return false; }
      }, 'daemon startup');
      return process;
    } catch (error) { await cleanup(process); throw error; }
  }
  async function stop(process) {
    process.child.kill('SIGTERM');
    const result = await wait(process);
    assert.equal(result.code, 0, result.stderr);
    assert.equal(existsSync(path), false, 'Daemon left its control socket');
  }
  const help = await run(binary, ['--help'], env);
  assert.equal(help.code, 0, help.stderr);
  assert.match(help.stdout, /pi-voice/);
  child = await launch();
  assert.equal((await stat(join(env.XDG_RUNTIME_DIR, 'pi-voice'))).mode & 0o777, 0o700);
  assert.equal((await stat(path)).mode & 0o777, 0o600);
  const status = await request(path, { action: 'status' });
  assert.equal(status.phase, 'idle');
  assert.equal(status.connection_state, 'unselected');
  assert.deepEqual(status.sessions, []);
  assert.equal(status.speech_available, false);
  for (const key of ['draft', 'pending', 'retained', 'retry', 'recording', 'transcribing', 'speaking']) {
    assert.equal(typeof status[key], 'boolean', key);
  }
  for (const action of ['record', 'send', 'read', 'unknown-command', 'fixture-state']) {
    assert.equal((await request(path, { action })).ok, false, action);
  }
  assert.equal((await request(path, { action: 'team-toggle' })).show_team, true);
  assert.equal((await stat(join(env.XDG_RUNTIME_DIR, 'pi-voice/selection.json'))).mode & 0o777, 0o600);
  const second = start(binary, ['serve'], env);
  try { assert.notEqual((await wait(second)).code, 0, 'Second daemon accepted the same runtime'); }
  finally { await cleanup(second); }
  assert.equal((await request(path, { action: 'status' })).ok, true);

  // Eight concurrent clients plus one incomplete request must remain responsive.
  const idle = net.createConnection(path);
  idle.on('error', () => {});
  try {
    await new Promise<void>(resolve => idle.once('connect', () => { idle.write('{"action":'); resolve(); }));
    await Promise.all(Array.from({ length: 8 }, async () => {
      for (let i = 0; i < 3; i++) assert.equal((await request(path, { action: 'status' })).ok, true);
    }));
    await stop(child);
    child = null;
  } finally { idle.destroy(); }

  child = await launch();
  assert.equal((await request(path, { action: 'status' })).show_team, true);
  for (let i = 0; i < 32; i++) {
    await new Promise<void>((resolve, reject) => {
      const gone = net.createConnection(path);
      gone.once('error', reject);
      gone.once('connect', () => gone.write('{"action":"status"}\n', () => { gone.destroy(); resolve(); }));
    });
  }
  // Only read-only probes may retry while a bounded server drains the burst.
  await until(async () => {
    assert.equal(child.output.done, false, child.output.stderr);
    try { return (await request(path, { action: 'status' })).ok; }
    catch { return false; }
  }, 'server recovered from disappearing clients', 3000);
  await stop(child);
  child = null;
  const observed = (await readFile(calls, 'utf8')).trim().split('\n').map(line => JSON.parse(line));
  assert.ok(observed.every(row => basename(row[0]) === 'pw-dump'), 'frontend must never manage backend services');

  const received = [];
  server = await listen(path, command => {
    received.push(command);
    if (command.action !== 'interact') return { ok: true };
  });
  const uncertain = await run(binary, ['interact'], env);
  assert.notEqual(uncertain.code, 0);
  assert.match(uncertain.stderr, /unknown/);
  assert.deepEqual(received.map(row => row.action), ['interact']);
  received.length = 0;
  const extension = join(root, '.pi/agent/extensions');
  await mkdir(extension, { recursive: true });
  await writeFile(join(extension, 'pi-voice.ts'), '// Disposable installation marker.\n');
  const piResult = join(root, 'pi-launch.json');
  await tool(join(env.PATH, 'pi'), `require('node:fs').writeFileSync(${JSON.stringify(piResult)}, JSON.stringify({
args: process.argv.slice(2), token: process.env.AGENT_VOICE_TOKEN, kind: process.env.AGENT_VOICE_KIND
}));\n`);
  const launched = await run(binary, ['--', '--native-smoke'], {
    ...env, HERDR_ENV: '1', HERDR_PANE_ID: 'disposable-pane', HERDR_SOCKET_PATH: join(root, 'fake-herdr.sock'),
  }, 10000);
  assert.equal(launched.code, 0, launched.stderr);
  assert.deepEqual(received.map(row => row.action), ['status', 'register', 'unregister']);
  const launchInfo = JSON.parse(await readFile(piResult, 'utf8'));
  assert.deepEqual(launchInfo.args, ['--native-smoke']);
  assert.equal(launchInfo.kind, 'pi');
  assert.equal(received[1].token, launchInfo.token);
  assert.equal(received[2].token, launchInfo.token);
  assert.equal(existsSync(join(env.XDG_RUNTIME_DIR, 'pi-voice', launchInfo.token)), false);
  console.log('Daemon: private IPC, persistence, concurrency, shutdown, launcher and no mutation replay');
} finally {
  await cleanup(child);
  if (server) await server.close();
  await rm(root, { recursive: true, force: true });
}
