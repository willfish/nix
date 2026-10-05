import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
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

async function fixture(t, options: { managed?: boolean; unready?: boolean; startFailure?: boolean; loading?: number } = {}) {
  const requests: { url: string; body: Buffer; headers: object }[] = [];
  let upstreamStatus = 200;
  let upstreamBody: Buffer | undefined;
  let delay = 0;
  let healthAttempts = 0;
  const upstream = createServer(async (req, res) => {
    if (req.method === 'GET') {
      const ready = !options.unready && healthAttempts++ >= (options.loading ?? 0);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(req.url === '/health' ? { status: ready ? 'ok' : 'loading' }
        : { data: [{ id: 'pi-voice', loaded: ready }] }));
      return;
    }
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
  const commandLog = join(dir, 'commands.jsonl');
  await writeFile(commandLog, '');
  await writeFile(join(dir, 'service.cjs'), `
    require('node:fs').appendFileSync(${JSON.stringify(commandLog)}, JSON.stringify(process.argv.slice(2)) + '\\n');
    if (${!!options.startFailure} && process.argv[2] === 'start') process.exit(1);
  `);
  await writeFile(join(dir, 'config.json'), JSON.stringify({
    port: apiPort, stt_url: `http://127.0.0.1:${port}/inference`,
    tts_url: `http://127.0.0.1:${port}/speech`,
    voices: { samantha: {}, 'samantha-long': { voice_ref: '/trusted/long.wav' }, data: { voice_ref: '/trusted/reference.wav', reference_text: 'Reference' } },
    queue_timeout: 0.1,
    ...(options.managed ? {
      systemctl: [process.execPath, join(dir, 'service.cjs')],
      readiness_timeout: 0.4, idle_timeout: 0.1, queue_timeout: 2,
      engines: {
        stt: { unit: 'fixture-stt.service', health_url: `http://127.0.0.1:${port}/health` },
        tts: { unit: 'fixture-tts.service', health_url: `http://127.0.0.1:${port}/models` },
      },
    } : {}),
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
  return { requests, base,
    async commands() { return (await readFile(commandLog, 'utf8')).trim().split('\n').filter(Boolean).map(line => JSON.parse(line)); },
    async stop() { const exit = once(child, 'exit'); child.kill(); await exit; },
    setReply(status: number, body?: Buffer, ms = 0) {
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

test('whole-reply context keeps long-reference policy in the backend', async t => {
  const f = await fixture(t);
  for (const text of ['First short chunk.', 'Second short chunk.']) {
    assert.equal((await f.post('/v1/speak?model=samantha', JSON.stringify({ text }), { 'X-Voice-Context-Words': '51' })).status, 200);
    assert.equal(JSON.parse(f.requests.at(-1).body.toString()).voice_ref, '/trusted/long.wav');
  }
  assert.equal((await f.post('/v1/speak?model=samantha', '{"text":"Short reply."}', { 'X-Voice-Context-Words': '50' })).status, 200);
  assert.equal(JSON.parse(f.requests.at(-1).body.toString()).voice_ref, undefined);
  for (const words of ['-1', 'invalid', '1000001'])
    assert.equal((await f.post('/v1/speak', '{"text":"Hello"}', { 'X-Voice-Context-Words': words })).status, 400);
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

test('backend owns service startup, readiness, voice policy and idle shutdown', async t => {
  const f = await fixture(t, { managed: true, loading: 2 });
  assert.equal((await f.post('/v1/listen', wav())).status, 200);
  assert.ok((await f.commands()).some(args => args[0] === 'start' && args[1] === 'fixture-stt.service'));
  assert.equal((await f.post('/v1/speak?model=samantha', JSON.stringify({ text: 'word '.repeat(51) }))).status, 200);
  assert.equal(JSON.parse(f.requests[1].body.toString()).voice_ref, '/trusted/long.wav');
  assert.ok((await f.commands()).some(args => args[0] === 'start' && args[1] === 'fixture-tts.service'));
  const deadline = Date.now() + 3000;
  while (Date.now() < deadline && !(await f.commands()).some(args => args[0] === 'stop' && args[1] === 'fixture-stt.service'))
    await new Promise(resolve => setTimeout(resolve, 30));
  assert.ok((await f.commands()).some(args => args[0] === 'stop' && args[1] === 'fixture-stt.service'));
  assert.equal((await f.post('/v1/listen', wav())).status, 200, 'idle engine restarts on demand');
  await f.stop();
  const commands = await f.commands();
  assert.equal(commands.at(-1)[0], 'stop', 'shutdown releases owned engines');
});

test('backend reports failed startup and readiness without sending inference', async t => {
  for (const options of [{ startFailure: true }, { unready: true }]) {
    await t.test(JSON.stringify(options), async t => {
      const f = await fixture(t, { managed: true, ...options });
      const response = await f.post('/v1/listen', wav());
      assert.equal(response.status, 503);
      assert.equal(f.requests.length, 0);
      assert.ok((await response.json()).err_code);
    });
  }
});

test('backend never stops an engine while its HTTP operation is active', async t => {
  const f = await fixture(t, { managed: true });
  f.setReply(200, undefined, 1500);
  const response = f.post('/v1/listen', wav());
  while (!f.requests.length) await new Promise(resolve => setTimeout(resolve, 5));
  await new Promise(resolve => setTimeout(resolve, 1100));
  assert.ok(!(await f.commands()).some(args => args[0] === 'stop'));
  assert.equal((await response).status, 200);
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
