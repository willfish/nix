import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import test from 'node:test';
import { gzipSync } from 'node:zlib';
import { invoke } from './helpers/voice-python-target.ts';

const url = 'http://127.0.0.1:8998';
const handler = '    async def handle_chat(self, request):\n        ws = web.WebSocketResponse()';
async function directory(t) {
  const root = await mkdtemp('/tmp/voice-models-');
  t.after(() => rm(root, { recursive: true, force: true }));
  return root;
}
function archive(name, symlink = false) {
  const header = Buffer.alloc(512);
  const field = (offset, size, value) => header.write(value, offset, size, 'ascii');
  field(0, 100, name); field(100, 8, '0000644\0');
  field(108, 8, '0000000\0'); field(116, 8, '0000000\0');
  field(124, 12, '00000000000\0'); field(136, 12, '00000000000\0');
  header.fill(32, 148, 156); field(156, 1, symlink ? '2' : '0');
  if (symlink) field(157, 100, '/tmp/escape');
  field(257, 6, 'ustar\0'); field(263, 2, '00');
  const sum = header.reduce((total, byte) => total + byte, 0);
  field(148, 8, sum.toString(8).padStart(6, '0') + '\0 ');
  return gzipSync(Buffer.concat([header, Buffer.alloc(1024)]));
}

test('model assets require both the expected hash and size', async t => {
  const path = join(await directory(t), 'model');
  await writeFile(path, 'abc');
  const digest = createHash('sha256').update('abc').digest('hex');
  assert.equal(invoke('matches', { path, size: 3, digest }).value, true);
  assert.equal(invoke('matches', { path, size: 4, digest }).value, false);
  assert.equal(invoke('matches', { path, size: 3, digest: '0'.repeat(64) }).value, false);
});
test('redirect drops credentials on another host', () => {
  const result = invoke('redirect', {
    url: 'https://huggingface.co/model', headers: { Authorization: 'Bearer private' }, to: 'https://cdn.example/model',
  });
  assert.deepEqual(result.value, { authorization: null, url: 'https://cdn.example/model' });
});
test('redirect rejects cleartext', () => {
  const result = invoke('redirect', { url: 'https://huggingface.co/model', to: 'http://cdn.example/model' });
  assert.equal(result.type, 'ValueError');
  assert.match(result.error, /non-HTTPS/);
});
test('check-only installation never reads credentials or downloads', async t => {
  const result = invoke('install', { root: await directory(t), check_only: true });
  assert.equal(result.type, 'ValueError');
  assert.match(result.error, /Missing or invalid/);
  assert.equal(result.token_reads, 0);
});
test('archive extraction rejects traversal and symlinks', async t => {
  for (const name of ['../escape', 'voices/../../escape', 'voices/link']) {
    const root = await directory(t);
    await writeFile(join(root, 'voices.tgz'), archive(name, name.endsWith('link')));
    const result = invoke('install', { root, assets: {} });
    assert.equal(result.type, 'ValueError', name);
    assert.match(result.error, /Unsafe member/, name);
    assert.equal(result.token_reads, 0);
  }
});
test('server patch fails closed when upstream changes', () => {
  const result = invoke('patch', { source: 'different source' });
  assert.equal(result.type, 'ValueError');
  assert.match(result.error, /changed/);
});
test('server guards reject untrusted requests before opening a websocket', () => {
  const tuples = [
    ['https://example.com', 'NATF2.pt', '', false],
    [null, 'NATF2.pt', '', false],
    [url, '../../secret.pt', '', false],
    [url, 'NATF2.pt', 'x'.repeat(8001), false],
    [url, 'NATF2.pt', '', true],
    [url, 'NATF2.pt', '', false],
  ];
  const requests = tuples.map(([origin, voice, prompt, busy]) => ({ origin, voice, prompt, busy }));
  const result = invoke('guards', { source: handler, requests });
  assert.deepEqual(result.value, [
    ...Array.from({ length: 5 }, () => ({ denied: true, websockets: 0 })),
    { denied: false, websockets: 1 },
  ]);
});
test('browser guard injection is explicit and fails on root drift', () => {
  const rejected = invoke('patch', { source: handler, guard: '/guard.js' });
  assert.equal(rejected.type, 'ValueError');
  assert.match(rejected.error, /root changed/);
  const root = '\n    def root(self):\n        return web.FileResponse(os.path.join(static_path, "index.html"))';
  const result = invoke('patch', { source: handler + root, guard: '/guard.js', compile: true });
  assert.equal(result.error, undefined);
  assert.ok(result.value.includes('Path("/guard.js").read_text()'));
  assert.ok(result.value.includes('content_type="text/html"'));
});
test('patched server guards origin, voice path and busy state', () => {
  const result = invoke('patch', { source: handler });
  for (const fragment of ['request.headers.get("Origin")', 'if voice not in allowed_voices:', 'if self.lock.locked():']) {
    assert.ok(result.value.includes(fragment), fragment);
  }
});
