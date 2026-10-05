import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createServer as createTlsServer } from 'node:https';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';

const fixture = process.env.VOICE_MODELS_FIXTURE;
const binary = process.env.VOICE_MODELS_BIN;
assert.ok(fixture && binary, 'Set VOICE_MODELS_FIXTURE and VOICE_MODELS_BIN');
const pinned = JSON.parse(readFileSync(fileURLToPath(new URL('./assets.json', import.meta.url)), 'utf8'));
const payload = Buffer.from('speech model fixture\n');
const digest = (data: any) => createHash('sha256').update(data).digest('hex');
function root(t: any) {
  const path = mkdtempSync(join(tmpdir(), 'voice-models-'));
  t.after(() => rmSync(path, { recursive: true, force: true })); return path;
}
async function invoke(command: string, args: string[], env: any = {}, cwd?: string) {
  return await new Promise<{ status: number | null, stdout: string, stderr: string }>((resolve, reject) => {
    const child = spawn(command, args, { env: { ...process.env, ...env }, cwd, stdio: ['ignore', 'pipe', 'pipe'] });
    let stdout = '', stderr = '';
    const timer = setTimeout(() => { child.kill('SIGKILL'); reject(new Error(`Fixture timed out: ${stdout} ${stderr}`)); }, 15000);
    child.stdout.on('data', chunk => stdout += chunk); child.stderr.on('data', chunk => stderr += chunk);
    child.on('error', error => { clearTimeout(timer); reject(error); });
    child.on('close', (status, signal) => { clearTimeout(timer); if (signal) reject(new Error(`${signal}: ${stderr}`)); else resolve({ status, stdout, stderr }); });
  });
}
async function server(t: any, handler: any, tls?: any) {
  const requests: any[] = [];
  const endpoint = tls ? createTlsServer(tls) : createServer();
  endpoint.on('request', (request: any, response: any) => { requests.push({ url: request.url, headers: request.headers }); handler(request, response, requests.length); });
  await new Promise<void>(resolve => endpoint.listen(0, '127.0.0.1', resolve));
  t.after(() => { endpoint.closeAllConnections(); endpoint.close(); });
  const address = endpoint.address() as any;
  return { url: `${tls ? 'https' : 'http'}://127.0.0.1:${address.port}/model`, requests };
}
function install(path: string, url: string, options: any = {}) {
  return invoke(fixture!, ['install', path, options.name || 'models/model.bin', url, options.hash || digest(payload), String(options.bytes ?? payload.length), String(options.idle ?? 2000), options.check ? '1' : '0'], options.env);
}
function noTemporary(path: string) {
  for (const entry of readdirSync(path, { recursive: true })) assert.ok(!String(entry).endsWith('.partial'), String(entry));
}

test('all asset URLs, paths, SHA-256 values, byte counts, order and flags remain pinned', async () => {
  const result = await invoke(fixture!, ['assets']); assert.equal(result.status, 0); assert.deepEqual(JSON.parse(result.stdout), pinned);
  const defaults = await invoke(fixture!, ['defaults']); assert.deepEqual(JSON.parse(defaults.stdout), { idle_ms: 30000, backoff_ms: 1000 });
});
test('host, STT and experimental filters retain the full selection matrix', async () => {
  for (const host of ['foundation', 'andromeda', 'Andromeda', 'other']) for (const stt of [false, true]) for (const experimental of [false, true]) {
    const selected = pinned.filter((asset: any) => (!stt || asset.role === 'stt') && (!asset.hosts || asset.hosts.includes(host)) && (!asset.experimental || experimental));
    const result = await invoke(fixture!, ['select', host, stt ? '1' : '0', experimental ? '1' : '0']); assert.equal(result.status, 0); assert.deepEqual(JSON.parse(result.stdout), selected);
  }
});
test('production CLI check-only resolves explicit, HOME, XDG and empty-XDG roots without creating directories', async t => {
  const home = root(t);
  for (const [args, env, expected] of [
    [['--data-dir', join(home, 'explicit')], { HOME: home }, join(home, 'explicit')],
    [[], { HOME: home, XDG_DATA_HOME: undefined }, join(home, '.local/share/pi-voice')],
    [[], { HOME: home, XDG_DATA_HOME: join(home, 'xdg') }, join(home, 'xdg/pi-voice')],
    [[], { HOME: home, XDG_DATA_HOME: '' }, 'pi-voice'],
  ] as any) {
    const result = await invoke(binary!, [...args, '--check-only', '--stt-only', '--host', 'foundation'], env, home);
    assert.equal(result.status, 1); assert.match(result.stderr, /Missing or invalid model:/); assert.ok(result.stderr.includes(`${expected}/models/ggml-silero-v6.2.0.bin`));
  }
  assert.deepEqual(readdirSync(home), []);
});
test('usage, help and unknown arguments do not download', async () => {
  assert.equal((await invoke(binary!, ['--help'])).status, 0);
  for (const args of [['--bad'], ['--host'], ['trailing'], ['--check-only=1']]) assert.equal((await invoke(binary!, args)).status, 2);
});
test('matching local size AND hash avoids requests and preserves existing permissions', async t => {
  const path = root(t); const destination = join(path, 'models/model.bin'); mkdirSync(join(path, 'models')); writeFileSync(destination, payload, { mode: 0o644 });
  const remote = await server(t, (_q: any, r: any) => r.end(payload));
  const result = await install(path, remote.url); assert.equal(result.status, 0); assert.equal(result.stdout, `Verified ${destination}\n`); assert.equal(remote.requests.length, 0); assert.equal(statSync(destination).mode & 0o777, 0o644);
});
test('check-only never creates directories or contacts the network for missing/wrong-size/wrong-hash models', async t => {
  const path = root(t), remote = await server(t, (_q: any, r: any) => r.end(payload));
  const missing = join(path, 'missing'); assert.equal((await install(missing, remote.url, { check: true })).status, 1); assert.equal(existsSync(missing), false);
  mkdirSync(join(path, 'models')); writeFileSync(join(path, 'models/model.bin'), payload);
  for (const options of [{ bytes: payload.length + 1 }, { hash: '0'.repeat(64) }]) assert.equal((await install(path, remote.url, { ...options, check: true })).status, 1);
  assert.equal(remote.requests.length, 0);
});
test('valid streams install atomically with exact messages, mode 0600 and no partial files', async t => {
  const path = root(t), remote = await server(t, (_q: any, r: any) => r.end(payload));
  const result = await install(path, remote.url); assert.equal(result.status, 0, result.stderr);
  const destination = join(path, 'models/model.bin'); assert.deepEqual(readFileSync(destination), payload); assert.equal(statSync(destination).mode & 0o777, 0o600); noTemporary(path);
  assert.equal(result.stdout, `Downloading models/model.bin (${payload.length} bytes)\nInstalled ${destination}\n`); assert.equal(remote.requests[0].headers['user-agent'], 'pi-voice-model-setup'); assert.equal(remote.requests[0].headers.connection, 'close'); assert.equal(remote.requests[0].headers['accept-encoding'], 'identity'); assert.equal(remote.requests[0].headers.accept, undefined);
});
test('downloaded models replace invalid symlinks, not the symlink target', async t => {
  const path = root(t), outside = join(path, 'outside'); writeFileSync(outside, 'not a model'); mkdirSync(join(path, 'models')); symlinkSync(outside, join(path, 'models/model.bin'));
  const remote = await server(t, (_q: any, r: any) => r.end(payload)); assert.equal((await install(path, remote.url)).status, 0); assert.equal(readFileSync(outside, 'utf8'), 'not a model'); assert.deepEqual(readFileSync(join(path, 'models/model.bin')), payload); noTemporary(path);
});
test('same-size bad hashes and oversize bodies fail without retrying or replacing prior data', async t => {
  for (const options of [{ hash: '0'.repeat(64) }, { bytes: 2 }]) {
    const path = root(t); mkdirSync(join(path, 'models')); writeFileSync(join(path, 'models/model.bin'), 'old');
    const remote = await server(t, (_q: any, r: any) => r.end(payload)); const result = await install(path, remote.url, options);
    assert.equal(result.status, 1); assert.match(result.stderr, /integrity check failed|exceeds expected size/); assert.equal(remote.requests.length, 1); assert.equal(readFileSync(join(path, 'models/model.bin'), 'utf8'), 'old'); noTemporary(path);
  }
});
test('short complete or truncated content-length bodies are integrity failures, not retry loops', async t => {
  for (const length of [undefined, payload.length]) {
    const path = root(t), remote = await server(t, (_q: any, r: any) => { if (length) { r.setHeader('Content-Length', length); r.setHeader('Connection', 'close'); } r.end(payload.subarray(0, 2)); });
    const result = await install(path, remote.url); assert.equal(result.status, 1); assert.match(result.stderr, /integrity check failed/); assert.equal(remote.requests.length, 1); noTemporary(path);
  }
});
test('valid pinned bytes remain authoritative when Content-Length overstates the body', async t => {
  const path = root(t), remote = await server(t, (_q: any, r: any) => { r.writeHead(200, { 'Content-Length': payload.length + 100, Connection: 'close' }); r.end(payload); });
  const result = await install(path, remote.url); assert.equal(result.status, 0, result.stderr); assert.equal(remote.requests.length, 1); assert.deepEqual(readFileSync(join(path, 'models/model.bin')), payload); noTemporary(path);
});
test('truncated chunk framing is rejected without retrying even when its bytes match the model', async t => {
  const path = root(t), remote = await server(t, (q: any) => { q.socket.end(`HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n${payload.length.toString(16)}\r\n${payload.toString()}\r\n`); });
  const result = await install(path, remote.url); assert.equal(result.status, 1); assert.equal(remote.requests.length, 1); assert.match(result.stderr, /incomplete chunked/); assert.equal(existsSync(join(path, 'models/model.bin')), false); noTemporary(path);
});
test('HTTP and transport failures retry three attempts, then preserve old data and clean every temporary', async t => {
  for (const kind of ['HTTP', 'reset']) {
    const path = root(t), remote = await server(t, (q: any, r: any) => { if (kind === 'reset') q.socket.destroy(); else { r.writeHead(503); r.end('unavailable'); } });
    const result = await install(path, remote.url); assert.equal(result.status, 1); assert.equal(remote.requests.length, 3); assert.equal(result.stdout.split('Downloading ').length - 1, 3); noTemporary(path);
    if (kind === 'HTTP') assert.match(result.stderr, /HTTP 503/);
  }
});
test('a retry can recover and commit only the successful download', async t => {
  const path = root(t), remote = await server(t, (_q: any, r: any, number: number) => { if (number < 3) { r.writeHead(502); r.end(); } else r.end(payload); });
  const result = await install(path, remote.url); assert.equal(result.status, 0, result.stderr); assert.equal(remote.requests.length, 3); assert.deepEqual(readFileSync(join(path, 'models/model.bin')), payload); noTemporary(path);
});
test('HTTP errors are recognized at headers without waiting on stalled error bodies', async t => {
  const path = root(t), remote = await server(t, (_q: any, r: any) => { r.writeHead(503, { 'Content-Length': 10000 }); r.flushHeaders(); });
  const start = Date.now(), result = await install(path, remote.url, { idle: 10000 }); assert.equal(result.status, 1); assert.equal(remote.requests.length, 3); assert.ok(Date.now() - start < 3000); noTemporary(path);
});
test('unhandled HTTP status and missing redirect locations retry without consuming bodies', async t => {
  for (const status of [300, 301, 304, 307]) {
    const path = root(t), remote = await server(t, (_q: any, r: any) => { r.writeHead(status, { 'Content-Length': 10000 }); r.flushHeaders(); });
    const start = Date.now(), result = await install(path, remote.url, { idle: 10000 });
    assert.equal(result.status, 1); assert.match(result.stderr, new RegExp(`HTTP ${status}`)); assert.equal(remote.requests.length, 3); assert.ok(Date.now() - start < 3000); noTemporary(path);
  }
});
test('redirects do not hash their bodies and only the final model is published', async t => {
  const path = root(t), remote = await server(t, (q: any, r: any) => { if (q.url === '/model') { r.writeHead(302, { Location: '/final' }); r.end('irrelevant oversized redirect body'); } else r.end(payload); });
  const result = await install(path, remote.url); assert.equal(result.status, 0, result.stderr); assert.deepEqual(remote.requests.map(q => q.url), ['/model', '/final']); assert.deepEqual(readFileSync(join(path, 'models/model.bin')), payload); noTemporary(path);
});
test('progressing transfers can outlive the idle timeout; stalled transfers time out and retry', async t => {
  const path = root(t), remote = await server(t, (_q: any, r: any) => {
    let index = 0; const timer = setInterval(() => { if (index === payload.length) { clearInterval(timer); r.end(); } else r.write(payload.subarray(index, ++index)); }, 60);
    r.on('close', () => clearInterval(timer));
  });
  const start = Date.now(), result = await install(path, remote.url, { idle: 300 }); assert.equal(result.status, 0, result.stderr); assert.ok(Date.now() - start > 1000); noTemporary(path);
  const stalledPath = root(t), stalled = await server(t, (_q: any, r: any) => { r.writeHead(200); r.flushHeaders(); });
  const failure = await install(stalledPath, stalled.url, { idle: 250 }); assert.equal(failure.status, 1); assert.equal(stalled.requests.length, 3); noTemporary(stalledPath);
});
test('directory destinations fail publication and retain their contents through all retries', async t => {
  const path = root(t); mkdirSync(join(path, 'models/model.bin'), { recursive: true }); writeFileSync(join(path, 'models/model.bin/keep'), 'keep');
  const remote = await server(t, (_q: any, r: any) => r.end(payload)); const result = await install(path, remote.url); assert.equal(result.status, 1); assert.equal(remote.requests.length, 3); assert.equal(readFileSync(join(path, 'models/model.bin/keep'), 'utf8'), 'keep'); noTemporary(path);
});
test('TLS requires declared CA trust and checks the destination hostname', async t => {
  const path = root(t), key = join(path, 'key.pem'), cert = join(path, 'cert.pem');
  const generated = spawnSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', cert, '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], { encoding: 'utf8' }); assert.equal(generated.status, 0, generated.stderr);
  const remote = await server(t, (_q: any, r: any) => r.end(payload), { key: readFileSync(key), cert: readFileSync(cert) });
  const untrusted = await install(join(path, 'untrusted'), remote.url, { env: { SSL_CERT_FILE: undefined } }); assert.equal(untrusted.status, 1);
  const trusted = await install(join(path, 'trusted'), remote.url, { env: { SSL_CERT_FILE: cert } }); assert.equal(trusted.status, 0, trusted.stderr);
  const missing = await install(join(path, 'missing-ca'), remote.url, { env: { SSL_CERT_FILE: join(path, 'absent.pem') } }); assert.equal(missing.status, 1);
  const wrongCert = join(path, 'wrong-cert.pem');
  const mismatch = spawnSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', wrongCert, '-days', '1', '-subj', '/CN=wrong.example', '-addext', 'subjectAltName=DNS:wrong.example'], { encoding: 'utf8' }); assert.equal(mismatch.status, 0, mismatch.stderr);
  const wrongHost = await server(t, (_q: any, r: any) => r.end(payload), { key: readFileSync(key), cert: readFileSync(wrongCert) });
  const rejected = await install(join(path, 'wrong-host'), wrongHost.url, { env: { SSL_CERT_FILE: wrongCert } }); assert.equal(rejected.status, 1); noTemporary(path);
});
