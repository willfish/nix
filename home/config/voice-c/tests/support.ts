// Shared fixture plumbing. Never inherit credentials or real service sockets.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { chmod, mkdir, writeFile } from 'node:fs/promises';
import net from 'node:net';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

export { delay };
export const maxFrame = 1024 * 1024;

export async function until(predicate, label, milliseconds = 12000) {
  const deadline = performance.now() + milliseconds;
  do {
    const value = await predicate();
    if (value) return value;
    await delay(40);
  } while (performance.now() < deadline);
  throw new Error(`Timed out: ${label}`);
}

export function start(binary, args, env) {
  const child = spawn(binary, args, { env, stdio: ['ignore', 'pipe', 'pipe'] });
  const output = { stdout: '', stderr: '', code: null, signal: null, done: false };
  child.stdout.on('data', data => { output.stdout += data; });
  child.stderr.on('data', data => { output.stderr += data; });
  child.on('error', error => { output.stderr += error.message; });
  const exited = new Promise(resolve => child.on('close', (code, signal) => {
    Object.assign(output, { code, signal, done: true });
    resolve(output);
  }));
  return { child, output, exited };
}

export async function wait(process, milliseconds = 5000) {
  const timer = setTimeout(() => process.child.kill('SIGKILL'), milliseconds);
  try {
    const output = await process.exited;
    assert.notEqual(output.signal, 'SIGKILL', `Child timed out: ${output.stderr}`);
    return output;
  } finally { clearTimeout(timer); }
}

export async function run(binary, args, env, milliseconds = 5000) {
  return wait(start(binary, args, env), milliseconds);
}

export async function cleanup(process) {
  if (!process) return;
  if (!process.output.done) process.child.kill('SIGKILL');
  await process.exited;
}

export function request(path, value, milliseconds = 3000) {
  return new Promise((resolve, reject) => {
    const client = net.createConnection(path);
    let bytes = Buffer.alloc(0), finished = false;
    const finish = (error, result?) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      client.destroy();
      error ? reject(error) : resolve(result);
    };
    const timer = setTimeout(() => finish(new Error('Socket request timed out')), milliseconds);
    client.once('connect', () => client.write(JSON.stringify(value) + '\n'));
    client.on('error', error => finish(error));
    client.on('end', () => finish(new Error('Response ended without a frame')));
    client.on('data', data => {
      bytes = Buffer.concat([bytes, data]);
      if (bytes.length > maxFrame) return finish(new Error('Oversized response'));
      const end = bytes.indexOf(10);
      if (end < 0) return;
      try { finish(null, JSON.parse(bytes.subarray(0, end).toString())); }
      catch (error) { finish(error); }
    });
  });
}

export async function listen(path, handler) {
  const clients = new Set<net.Socket>();
  const errors: Error[] = [];
  const server = net.createServer(client => {
    clients.add(client);
    client.on('close', () => clients.delete(client));
    client.on('error', () => {}); // Deliberately disappearing clients are fixtures.
    client.setTimeout(3000, () => client.destroy());
    let data = Buffer.alloc(0), handled = false;
    client.on('data', chunk => {
      if (handled) return;
      data = Buffer.concat([data, chunk]);
      if (data.length > maxFrame) return client.destroy();
      const end = data.indexOf(10);
      if (end < 0) return;
      handled = true;
      Promise.resolve().then(() => handler(JSON.parse(data.subarray(0, end).toString())))
        .then(reply => client.end(reply === undefined ? '' : JSON.stringify(reply) + '\n'))
        .catch(error => { errors.push(error); client.destroy(); });
    });
  });
  await new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(path, () => { server.removeListener('error', reject); resolve(); });
  });
  await chmod(path, 0o600);
  return {
    errors,
    async close() {
      for (const client of clients) client.destroy();
      await new Promise<void>(resolve => server.close(() => resolve()));
      assert.deepEqual(errors, [], 'Fixture server failed');
    },
  };
}

export async function tool(path, source) {
  await writeFile(path, `#!${process.execPath}\n${source}`, { mode: 0o700 });
}

export async function environment(root, config) {
  const runtime = join(root, 'run'), tools = join(root, 'bin');
  await mkdir(runtime, { mode: 0o700 });
  await mkdir(tools, { mode: 0o700 });
  const configPath = join(root, 'config.json');
  await writeFile(configPath, JSON.stringify({
    tts_enabled: false, auto_speak: false,
    voice_preferences_path: join(root, 'voice-mode'), ...config,
  }));
  return {
    HOME: root, XDG_RUNTIME_DIR: runtime, PI_VOICE_CONFIG: configPath,
    PATH: tools, LANG: 'C.UTF-8', LC_ALL: 'C.UTF-8',
  };
}
