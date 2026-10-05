import assert from 'node:assert/strict';
import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer, request } from 'node:http';
import { connect } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

// Always test the candidate build, never a binary from the active user profile.
const system = `${process.arch === 'arm64' ? 'aarch64' : 'x86_64'}-${process.platform === 'darwin' ? 'darwin' : 'linux'}`;
const binary = process.env.PI_VOICE_API_TEST_BIN ?? join(execFileSync('nix', [
  'build', '--no-link', '--print-out-paths', `.#checks.${system}.voice-api`,
], { encoding: 'utf8' }).trim(), 'bin/pi-voice-api');

function wav() {
  const b = Buffer.alloc(48);
  b.write('RIFF'); b.writeUInt32LE(40, 4); b.write('WAVEfmt ', 8);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22);
  b.writeUInt32LE(24000, 24); b.writeUInt32LE(48000, 28);
  b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34);
  b.write('data', 36); b.writeUInt32LE(4, 40);
  return b;
}

async function fixture(t, options: {
  managed?: boolean; unready?: boolean; startFailure?: boolean; loading?: number;
  stopDelay?: number; stopFailure?: boolean; env?: NodeJS.ProcessEnv;
} = {}) {
  const requests: { url: string; body: Buffer; headers: object }[] = [];
  let upstreamStatus = 200;
  let upstreamBody: Buffer | undefined;
  let upstreamHeaders = {};
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
    res.writeHead(upstreamStatus, upstreamHeaders);
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
    if (process.argv[2] === 'stop') {
      if (${!!options.stopFailure} && !require('node:fs').existsSync(${JSON.stringify(join(dir, 'stop-failed'))})) {
        require('node:fs').writeFileSync(${JSON.stringify(join(dir, 'stop-failed'))}, '');
        process.exit(1);
      }
      setTimeout(() => {}, ${options.stopDelay ?? 0});
    }
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
  const child = spawn(binary, ['--config', join(dir, 'config.json')], {
    stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...options.env },
  });
  let errors = '';
  let output = '';
  child.stderr.on('data', chunk => { errors += chunk; });
  child.stdout.on('data', chunk => { output += chunk; });
  t.after(async () => {
    if (child.exitCode === null && child.signalCode === null) {
      const exit = once(child, 'exit'); child.kill(); await exit;
    }
    assert.equal(errors, '', 'backend must not log requests or tracebacks');
    assert.equal(output, '', 'backend must not log transcripts to stdout');
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
    setReply(status: number, body?: Buffer, ms = 0, headers = {}) {
    upstreamStatus = status; upstreamBody = body; delay = ms; upstreamHeaders = headers;
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

function raw(base: string, message: string): Promise<string> {
  return new Promise((resolve, reject) => {
    const url = new URL(base);
    const socket = connect(Number(url.port), url.hostname);
    let response = '';
    socket.setTimeout(2000, () => socket.destroy(new Error('response timed out')));
    socket.on('connect', () => socket.write(message));
    socket.on('data', chunk => { response += chunk; });
    socket.on('end', () => resolve(response));
    socket.on('error', reject);
  });
}

test('HTTP framing is bounded and ambiguous requests never reach inference', async t => {
  const f = await fixture(t);
  for (const [headers, status] of [
    ['', 411],
    ['Content-Length: 0\r\n', 413],
    ['Content-Length: 1\r\nContent-Length: 2\r\n', 400],
    ['Content-Length: nope\r\n', 400],
    ['Transfer-Encoding: chunked\r\n', 403],
  ] as const) {
    const response = await raw(f.base, `POST /v1/listen HTTP/1.1\r\nHost: localhost\r\n${headers}\r\n`);
    assert.match(response, new RegExp(`^HTTP/1.1 ${status} `));
  }
  assert.equal(f.requests.length, 0);
  assert.equal((await f.post('/v1/listen', wav())).status, 200);
});

test('requests ignore proxy environment and never forward credentials or follow redirects', async t => {
  const f = await fixture(t, { env: {
    http_proxy: 'http://127.0.0.1:1', HTTP_PROXY: 'http://127.0.0.1:1',
    all_proxy: 'http://127.0.0.1:1', ALL_PROXY: 'http://127.0.0.1:1', no_proxy: '', NO_PROXY: '',
  } });
  assert.equal((await f.post('/v1/listen', wav(), { Authorization: 'Token private', Cookie: 'private' })).status, 200);
  assert.equal(f.requests[0].headers['authorization'], undefined);
  assert.equal(f.requests[0].headers['cookie'], undefined);
  for (const status of [301, 302, 307, 308]) {
    f.setReply(status, Buffer.from('private redirect'), 0, { Location: '/inference' });
    assert.equal((await f.post('/v1/listen', wav())).status, 502);
  }
  assert.equal(f.requests.length, 5, 'one request per call, no redirect follow-up');
});

test('malformed WAV formats and truncated output are rejected', async t => {
  const f = await fixture(t);
  for (const bytes of [wav().subarray(0, 45), Buffer.from('RIFF')]) {
    assert.equal((await f.post('/v1/listen', bytes)).status, 400);
    f.setReply(200, bytes);
    assert.equal((await f.post('/v1/speak', '{"text":"Hello"}')).status, 502);
  }
  for (const [offset, value] of [[20, 3], [22, 0], [22, 2], [34, 8]] as const) {
    const bytes = wav(); bytes.writeUInt16LE(value, offset);
    assert.equal((await f.post('/v1/listen', bytes)).status, 400);
  }
  const wrongRate = wav(); wrongRate.writeUInt32LE(48000, 24);
  f.setReply(200, wrongRate);
  assert.equal((await f.post('/v1/speak', '{"text":"Hello"}')).status, 502);
});

test('oversized upstream responses fail without leaking their contents', async t => {
  const f = await fixture(t);
  f.setReply(200, Buffer.alloc(32 * 1024 * 1024 + 1, 'x'));
  const response = await f.post('/v1/listen', wav());
  assert.equal(response.status, 502);
  assert.ok((await response.text()).length < 200);
});

test('shutdown drains active inference before stopping owned services', async t => {
  const f = await fixture(t, { managed: true });
  f.setReply(200, undefined, 200);
  const result = f.post('/v1/listen', wav());
  while (!f.requests.length) await new Promise(resolve => setTimeout(resolve, 5));
  const stopped = f.stop();
  assert.equal((await result).status, 200);
  await stopped;
  const commands = await f.commands();
  assert.ok(commands.some(args => args[0] === 'stop' && args[1] === 'fixture-stt.service'));
  assert.ok(commands.some(args => args[0] === 'stop' && args[1] === 'fixture-tts.service'));
});

test('idle stop commands do not block health requests and failed stops are retried', async t => {
  const f = await fixture(t, { managed: true, stopDelay: 400, stopFailure: true });
  const deadline = Date.now() + 4000;
  while (Date.now() < deadline && (await f.commands()).filter(args => args[0] === 'stop').length < 2)
    await new Promise(resolve => setTimeout(resolve, 10));
  assert.equal((await fetch(f.base + '/health', { signal: AbortSignal.timeout(300) })).status, 200);
  while (Date.now() < deadline && (await f.commands()).filter(args => args[0] === 'stop' && args[1] === 'fixture-stt.service').length < 2)
    await new Promise(resolve => setTimeout(resolve, 10));
  assert.ok((await f.commands()).filter(args => args[0] === 'stop' && args[1] === 'fixture-stt.service').length >= 2);
});

test('invalid configuration fails before serving and never logs URLs or reference paths', async t => {
  const dir = await mkdtemp(join(tmpdir(), 'voice-api-config-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  for (const override of [
    { stt_url: 'http://example.com/private' },
    { tts_url: 'http://private:secret@127.0.0.1/private' },
    { stt_url: 'https://127.0.0.1/private' },
    { tts_url: 'http://127.0.0.1:0/private' },
    { queue_timeout: -1 },
    { systemctl: [] },
    { engines: { stt: { unit: 'fixture-stt.service', health_url: 'http://example.com/private' } } },
  ]) {
    const path = join(dir, 'config.json');
    await writeFile(path, JSON.stringify({ stt_url: 'http://127.0.0.1/stt', tts_url: 'http://127.0.0.1/tts', ...override }));
    const child = spawn(binary, ['--config', path]);
    let error = ''; child.stderr.on('data', chunk => { error += chunk; });
    assert.equal((await once(child, 'exit'))[0], 1);
    assert.equal(error, 'pi-voice-api: invalid configuration\n');
  }
});
