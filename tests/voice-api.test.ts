import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { createServer, request } from 'node:http';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

function wav() {
  const b = Buffer.alloc(48);
  b.write('RIFF'); b.writeUInt32LE(40, 4); b.write('WAVEfmt ', 8);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(24000, 24); b.writeUInt32LE(48000, 28);
  b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36); b.writeUInt32LE(4, 40);
  return b;
}

async function fixture(t) {
  const requests: { url: string; body: Buffer; headers: object }[] = [];
  let upstreamStatus = 200;
  let upstreamBody: Buffer | undefined;
  let delay = 0;
  const upstream = createServer(async (req, res) => {
    const parts = [];
    for await (const chunk of req) parts.push(chunk);
    requests.push({ url: req.url!, body: Buffer.concat(parts), headers: req.headers });
    await new Promise(resolve => setTimeout(resolve, delay));
    res.writeHead(upstreamStatus);
    res.end(upstreamBody ?? (req.url === '/inference' ? '{"text":"Hello world"}' : wav()));
  });
  upstream.listen(0, '127.0.0.1');
  await once(upstream, 'listening');
  t.after(() => new Promise<void>(resolve => { upstream.closeAllConnections(); upstream.close(() => resolve()); }));
  const port = (upstream.address() as { port: number }).port;
  const reserve = createServer();
  reserve.listen(0, '127.0.0.1');
  await once(reserve, 'listening');
  const apiPort = (reserve.address() as { port: number }).port;
  await new Promise<void>(resolve => reserve.close(() => resolve()));
  const dir = await mkdtemp(join(tmpdir(), 'voice-api-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  await writeFile(join(dir, 'config.json'), JSON.stringify({
    port: apiPort, stt_url: `http://127.0.0.1:${port}/inference`,
    tts_url: `http://127.0.0.1:${port}/speech`,
    voices: { samantha: {}, data: { voice_ref: '/trusted/reference.wav', reference_text: 'Reference' } },
  }));
  const child = spawn('python3', ['home/config/voice/voice_api.py', '--config', join(dir, 'config.json')], { stdio: ['ignore', 'pipe', 'pipe'] });
  let errors = '';
  child.stderr.on('data', chunk => { errors += chunk; });
  t.after(async () => {
    if (child.exitCode === null && child.signalCode === null) {
      const exit = once(child, 'exit'); child.kill(); await exit;
    }
    assert.equal(errors, '', 'backend must not log requests or tracebacks');
  });
  const base = `http://127.0.0.1:${apiPort}`;
  let ready = false;
  for (let i = 0; i < 100; i++) {
    try { ready = (await fetch(base + '/health')).ok; } catch {}
    if (ready) break;
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  assert.ok(ready, errors);
  return { requests, base, setReply(status: number, body?: Buffer, ms = 0) {
    upstreamStatus = status; upstreamBody = body; delay = ms;
  }, post(path: string, body: Buffer | string, headers = {}) {
    return fetch(base + path, { method: 'POST', body, headers });
  } };
}

test('local listen accepts WAV and returns the Deepgram transcript envelope', async t => {
  const f = await fixture(t);
  const response = await f.post('/v1/listen?model=whisper&language=en&keyterm=NixOS&keyterm=Herdr', wav());
  assert.equal(response.status, 200);
  assert.deepEqual(await response.json(), { results: { channels: [{ alternatives: [{ transcript: 'Hello world' }] }] } });
  assert.equal(f.requests.length, 1);
  const request = f.requests[0];
  assert.equal(request.url, '/inference');
  assert.match(request.headers['content-type'], /^multipart\/form-data; boundary=/);
  assert.ok(request.body.includes(wav()));
  assert.match(request.body.toString(), /NixOS, Herdr/);
  assert.match(request.body.toString(), /name="temperature"\r\n\r\n0/);
  assert.equal(request.headers['authorization'], undefined);
});

test('local speak accepts Deepgram text, uses configured references, and returns WAV', async t => {
  const f = await fixture(t);
  const response = await f.post('/v1/speak?model=data&encoding=linear16&container=wav&sample_rate=24000',
    JSON.stringify({ text: 'Hello', voice_ref: '/untrusted.wav' }));
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('content-type'), 'audio/wav');
  assert.deepEqual(Buffer.from(await response.arrayBuffer()), wav());
  assert.deepEqual(JSON.parse(f.requests[0].body.toString()), {
    input: 'Hello', model: 'pi-voice', language: 'English',
    voice_ref: '/trusted/reference.wav', reference_text: 'Reference',
  });
});

test('bad input, unsupported formats, browser requests and unknown routes are rejected', async t => {
  const f = await fixture(t);
  for (const [path, body, status, headers] of [
    ['/v1/listen', 'not audio', 400, {}],
    ['/v1/speak', 'invalid json', 400, {}],
    ['/v1/speak', '[]', 400, {}],
    ['/v1/speak', '{"text":" "}', 400, {}],
    ['/v1/speak?model=unknown', '{"text":"hello"}', 400, {}],
    ['/v1/speak?encoding=mp3', '{"text":"hello"}', 400, {}],
    ['/v1/speak?sample_rate=48000', '{"text":"hello"}', 400, {}],
    ['/v1/speak', '{"text":"hello"}', 403, { Origin: 'https://example.test' }],
    ['/unknown', '{}', 404, {}],
  ] as const) {
    const response = await f.post(path, body, headers);
    assert.equal(response.status, status, path + body);
    assert.ok((await response.json()).err_code);
  }
  // Send only oversized headers: the server must reject before reading a body.
  const status = await new Promise<number | undefined>((resolve, reject) => {
    const req = request(f.base + '/v1/listen', { method: 'POST', headers: {
      'Content-Length': String(16 * 1024 * 1024 + 1),
    } }, res => { res.resume(); resolve(res.statusCode); });
    req.on('error', reject);
    req.end();
  });
  assert.equal(status, 413);
  assert.equal(f.requests.length, 0);
});

test('upstream errors and malformed output fail explicitly without fallback', async t => {
  const f = await fixture(t);
  for (const [path, input, status, output] of [
    ['/v1/listen', wav(), 500, Buffer.from('private engine error')],
    ['/v1/listen', wav(), 200, Buffer.from('not JSON')],
    ['/v1/listen', wav(), 200, Buffer.from('{}')],
    ['/v1/speak', '{"text":"Hello"}', 200, Buffer.from('not WAV')],
  ] as const) {
    f.setReply(status, output);
    const response = await f.post(path, input);
    assert.equal(response.status, 502);
    assert.ok(!(await response.text()).includes('private engine error'));
  }
  assert.equal(f.requests.length, 4);
});

test('concurrent inference is bounded and recovers after completion', async t => {
  const f = await fixture(t);
  f.setReply(200, undefined, 200);
  const first = f.post('/v1/listen', wav());
  while (!f.requests.length) await new Promise(resolve => setTimeout(resolve, 5));
  assert.equal((await f.post('/v1/listen', wav())).status, 429);
  assert.equal((await first).status, 200);
  assert.equal((await f.post('/v1/listen', wav())).status, 200);
});
