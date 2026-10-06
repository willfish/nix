import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import { once } from 'node:events';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { promisify } from 'node:util';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';

const execute = promisify(execFile);
const adapter = process.env.CAPTURE_MITM_BIN;
assert.ok(adapter, 'set CAPTURE_MITM_BIN to the compiled prompt-capture-mitm');
const source = fs.readFileSync(new URL('../../user/prompt-capture.sh', import.meta.url), 'utf8');
async function listen(server) {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  return server.address().port;
}
async function fixture(t, body = true) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'capture-native-'));
  const reserve = net.createServer();
  const port = await listen(reserve);
  await new Promise(resolve => reserve.close(resolve));
  const script = path.join(root, 'capture.sh');
  fs.writeFileSync(script, source.replace('pi) port=8302', `pi) port=${port}`));
  const env = { ...process.env, MITMDUMP: adapter, FLOCK: 'flock',
    XDG_STATE_HOME: root, PROMPT_CAPTURE_RESPONSE_BODY: body ? '1' : '0' };
  delete env.PROMPT_CAPTURE_UPSTREAM;
  const cdir = path.join(root, 'prompt-capture');
  const ready = path.join(root, 'ready');
  const children = [];
  const servers = [];
  const call = (...args) => execute('bash', [script, ...args], { env, timeout: 20000 });
  function kill(child, signal = 'SIGTERM') {
    try { process.kill(-child.pid, signal); } catch (e) { if (e.code !== 'ESRCH') throw e; }
  }
  async function stop(child) {
    kill(child);
    for (let i = 0; i < 100 && child.exitCode === null && child.signalCode === null; i++) await delay(50);
    kill(child, 'SIGKILL');
  }
  t.after(async () => {
    for (const child of children) await stop(child);
    for (const { server, sockets } of servers) {
      for (const socket of sockets) socket.destroy();
      server.closeAllConnections?.();
      await new Promise(resolve => server.close(resolve));
    }
    const log = path.join(cdir, 'pi-server.log');
    if (fs.existsSync(log)) assert.doesNotMatch(fs.readFileSync(log, 'utf8'), /addon error|native capture adapter failed/);
    fs.rmSync(root, { recursive: true, force: true });
  });
  async function start() {
    fs.rmSync(ready, { force: true });
    const child = spawn('bash', [script, 'pi', '--', process.execPath, '-e',
      `require('node:fs').writeFileSync(process.argv[1], JSON.stringify({base:process.env.ANTHROPIC_BASE_URL || '',umask:process.umask()}));setTimeout(()=>{},60000)`, ready],
      { env, detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
    children.push(child);
    let stderr = '';
    child.stderr.on('data', chunk => stderr += chunk);
    for (let i = 0; i < 300 && !fs.existsSync(ready); i++) {
      if (child.exitCode !== null || child.signalCode !== null) {
        const log = path.join(cdir, 'pi-server.log');
        assert.fail(stderr + (fs.existsSync(log) ? fs.readFileSync(log, 'utf8') : ''));
      }
      await delay(50);
    }
    assert.ok(fs.existsSync(ready), 'wrapped command did not start');
    return child;
  }
  async function server(handler, reverse = false) {
    const server = http.createServer(handler);
    const sockets = new Set();
    server.on('connection', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
    servers.push({ server, sockets });
    const upstreamPort = await listen(server);
    if (reverse) env.PROMPT_CAPTURE_UPSTREAM = `http://127.0.0.1:${upstreamPort}`;
    return { server, upstreamPort };
  }
  function request(upstreamPort, body = '', headers = {}, pathname = '/events') {
    return new Promise((resolve, reject) => {
      const req = http.request({ host: '127.0.0.1', port, method: body ? 'POST' : 'GET',
        path: env.PROMPT_CAPTURE_UPSTREAM ? pathname : `http://127.0.0.1:${upstreamPort}${pathname}`,
        headers: { ...headers, 'content-length': Buffer.byteLength(body) } }, resolve);
      req.on('error', reject); req.end(body);
    });
  }
  async function read(response) {
    const chunks = []; for await (const chunk of response) chunks.push(chunk);
    return Buffer.concat(chunks);
  }
  function records() {
    const log = path.join(cdir, 'pi.jsonl');
    return fs.readFileSync(log, 'utf8').trim().split('\n').filter(Boolean).map(line => JSON.parse(line));
  }
  return { root, port, ready, cdir, env, call, start, stop, server, request, read, records };
}

test('streaming preserves split UTF-8 and forwards original bytes immediately', async t => {
  const f = await fixture(t);
  let finish;
  const released = new Promise(resolve => finish = resolve);
  const { upstreamPort } = await f.server(async (_req, res) => {
    res.writeHead(200, { 'content-type': 'text/event-stream; charset=utf-8' });
    res.write(Buffer.from([100, 97, 116, 97, 58, 32, 0xe2]));
    await released;
    res.end(Buffer.from([0x82, 0xac, 10, 10]));
  });
  t.after(() => finish());
  await f.start();
  const response = await f.request(upstreamPort);
  const iterator = response[Symbol.asyncIterator]();
  const first = await iterator.next();
  assert.deepEqual(first.value, Buffer.from([100, 97, 116, 97, 58, 32, 0xe2]));
  finish();
  const chunks = [first.value];
  for await (const chunk of { [Symbol.asyncIterator]: () => iterator }) chunks.push(chunk);
  assert.equal(Buffer.concat(chunks).toString(), 'data: €\n\n');
  assert.equal(f.records().filter(r => r.kind === 'response_chunk').map(r => r.response_body).join(''), 'data: €\n\n');
});

test('large requests remain complete with metrics and redacted credentials', async t => {
  const f = await fixture(t);
  const { upstreamPort } = await f.server((req, res) => { req.resume(); req.on('end', () => res.end('{}')); });
  await f.start();
  const body = JSON.stringify({ model: 'qwen', messages: [{ content: '€'.repeat(210000) }], tools: [{ name: 'read' }] });
  await f.read(await f.request(upstreamPort, body, { Authorization: 'fixture-secret', 'x-api-key': 'fixture-secret', Cookie: 'fixture-secret', 'content-type': 'application/json' }));
  const record = f.records().find(r => r.kind === 'request');
  assert.equal(record.request_body, body);
  assert.equal(record.request_chars, body.length);
  assert.equal(record.request_bytes, Buffer.byteLength(body));
  assert.equal(record.model, 'qwen'); assert.equal(record.message_count, 1); assert.equal(record.tool_count, 1);
  assert.equal(record.request_headers.Authorization, '<redacted>');
  assert.doesNotMatch(JSON.stringify(f.records()), /fixture-secret/);
});

test('streaming without body capture omits response content', async t => {
  const f = await fixture(t, false);
  const { upstreamPort } = await f.server((_req, res) => {
    res.writeHead(200, { 'content-type': 'text/event-stream' }); res.end('data: {"private":"response"}\n\n');
  });
  await f.start();
  assert.equal((await f.read(await f.request(upstreamPort))).toString(), 'data: {"private":"response"}\n\n');
  assert.ok(f.records().every(r => !('response_body' in r)));
});

test('Anthropic usage survives chunking without logging response bodies', async t => {
  const f = await fixture(t, false);
  const events = 'event: message_start\r\ndata: {"message":{"usage":{"input_tokens":123,"output_tokens":0}}}\r\n\r\ndata: {"usage":{"output_tokens":7}}\n\n';
  const { upstreamPort } = await f.server(async (_req, res) => {
    res.writeHead(200, { 'content-type': 'text/event-stream' });
    res.write(events.slice(0, 31)); await delay(50); res.write(events.slice(31, 100)); await delay(50); res.end(events.slice(100));
  });
  await f.start();
  assert.equal((await f.read(await f.request(upstreamPort))).toString(), events);
  assert.deepEqual(f.records().filter(r => r.kind === 'usage').map(r => r.usage), [{ input_tokens: 123, output_tokens: 0 }, { output_tokens: 7 }]);
  assert.ok(f.records().every(r => !('response_body' in r)));
});

test('reverse proxy forwards local requests and records nonstreaming usage', async t => {
  const f = await fixture(t);
  let received = '';
  const { upstreamPort } = await f.server((req, res) => {
    assert.equal(req.url, '/v1/messages');
    req.on('data', chunk => received += chunk);
    req.on('end', () => { res.setHeader('content-type', 'application/json'); res.end('{"usage":{"input_tokens":321,"output_tokens":5}}'); });
  }, true);
  const child = await f.start();
  assert.equal(JSON.parse(fs.readFileSync(f.ready, 'utf8')).base, `http://127.0.0.1:${f.port}`);
  const body = '{"model":"qwen","messages":[{"role":"user","content":"hello"}]}';
  const response = await f.request(upstreamPort, body, { 'x-api-key': 'fixture-secret' }, '/v1/messages');
  assert.equal(response.statusCode, 200); await f.read(response);
  assert.equal(received, body);
  assert.equal(f.records().find(r => r.kind === 'usage').usage.input_tokens, 321);
  assert.doesNotMatch(JSON.stringify(f.records()), /fixture-secret/);
  await f.stop(child);
});

test('SSE reaches the client before the upstream response finishes', async t => {
  const f = await fixture(t);
  let finish;
  const release = new Promise(resolve => finish = resolve);
  t.after(() => finish());
  const { upstreamPort } = await f.server(async (_req, res) => {
    res.writeHead(200, { 'content-type': 'text/event-stream', 'content-length': 27 });
    res.write('data: first\n\n'); await release; res.end('data: second\n\n');
  });
  await f.start();
  const response = await f.request(upstreamPort);
  const iterator = response[Symbol.asyncIterator]();
  const first = await iterator.next();
  assert.equal(first.value.toString(), 'data: first\n\n');
  finish();
  const chunks = [first.value]; for await (const chunk of { [Symbol.asyncIterator]: () => iterator }) chunks.push(chunk);
  assert.equal(Buffer.concat(chunks).toString(), 'data: first\n\ndata: second\n\n');
  assert.equal(f.records().filter(r => r.kind === 'response_chunk').map(r => r.response_body).join(''), Buffer.concat(chunks).toString());
});

test('overlapping capture leaves the original proxy and lock intact', async t => {
  const f = await fixture(t); const first = await f.start();
  const pidfile = path.join(f.cdir, 'pi.pid'); const pid = fs.readFileSync(pidfile, 'utf8');
  await assert.rejects(f.call('pi', '--', 'true'), e => /already active/.test(e.stderr));
  assert.equal(fs.readFileSync(pidfile, 'utf8'), pid); process.kill(Number(pid), 0);
  assert.equal(first.exitCode, null); await f.stop(first); await f.start();
});

test('private state permissions are repaired and trust bundles contain no keys', async t => {
  const f = await fixture(t);
  fs.mkdirSync(f.cdir, { mode: 0o755 });
  for (const name of ['pi.jsonl', 'pi-ca.pem', 'pi-server.log']) fs.writeFileSync(path.join(f.cdir, name), '', { mode: 0o644 });
  await f.start();
  assert.equal(fs.statSync(f.cdir).mode & 0o777, 0o700);
  for (const name of ['pi.jsonl', 'pi-ca.pem', 'pi-server.log', 'pi.pid']) assert.equal(fs.statSync(path.join(f.cdir, name)).mode & 0o777, 0o600);
  const bundle = fs.readFileSync(path.join(f.cdir, 'pi-ca.pem'), 'utf8');
  assert.match(bundle, /BEGIN CERTIFICATE/); assert.doesNotMatch(bundle, /PRIVATE KEY/);
  assert.equal(JSON.parse(fs.readFileSync(f.ready, 'utf8')).umask, process.umask());
});

test('a busy port cannot launch the wrapped command', async t => {
  const f = await fixture(t); const busy = net.createServer(socket => socket.end());
  busy.listen(f.port, '127.0.0.1'); await once(busy, 'listening');
  t.after(() => busy.close());
  await assert.rejects(f.call('pi', '--', 'touch', path.join(f.root, 'ran')), e => /already in use/.test(e.stderr));
  assert.equal(fs.existsSync(path.join(f.root, 'ran')), false);
});

test('stop cleans up a stranded server and permits a new capture', async t => {
  const f = await fixture(t); const first = await f.start();
  first.kill('SIGKILL'); await once(first, 'exit');
  await assert.rejects(f.call('pi', '--', 'true'));
  await f.call('stop', 'pi'); await f.start();
});

test('WebSocket text and binary messages keep direction and representation', async t => {
  const f = await fixture(t);
  const { server } = await f.server((_req, res) => res.end(), true);
  server.on('upgrade', (req, socket) => {
    const accept = createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
    socket.once('data', () => { socket.write(Buffer.from([0x81, 5, ...Buffer.from('world'), 0x82, 2, 0, 255])); });
    t.after(() => socket.destroy());
  });
  await f.start();
  const ws = new WebSocket(`ws://127.0.0.1:${f.port}/socket`);
  t.after(() => ws.close());
  await once(ws, 'open');
  const received = new Promise(resolve => { let count = 0; ws.addEventListener('message', () => { if (++count === 2) resolve(); }); });
  ws.send('hello'); await received;
  const rows = f.records().filter(r => r.kind.startsWith('ws_'));
  assert.deepEqual(rows.map(r => [r.kind, r.data]), [['ws_request', 'hello'], ['ws_response', 'world'], ['ws_response', "b'\\x00\\xff'"]]);
});
