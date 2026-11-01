// Manual offline regressions for opt-in speech; no real credentials or speech API.
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile, appendFile, readdir, rm } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const plugin = resolve(process.argv[2] || 'home/config/opencode/harness');
const { spokenSummary, createSummarySpeaker, setupVoiceCommand } = await import(pathToFileURL(join(plugin, 'auto-speak.ts')));
const { speakText, setupSpeak } = await import(pathToFileURL(join(plugin, 'speak.ts')));

// Each worker runs the production queue and player in its own Node process.
if (process.argv[3] === '--queue-worker') {
  const controller = new AbortController();
  process.on('SIGTERM', () => controller.abort());
  const result = await speakText({ text: process.env.OPENCODE_SPEECH_FIXTURE_NAME }, {
    signal: controller.signal,
    maxWaitMs: Number(process.argv[4]),
    fetch: async () => {
      await appendFile(process.env.OPENCODE_SPEECH_FIXTURE_EVENTS, `fetch ${process.env.OPENCODE_SPEECH_FIXTURE_NAME}\n`);
      return new Response(Buffer.alloc(4800));
    },
  }).catch(error => { if (error.name === 'AbortError') return { cancelled: true }; throw error; });
  console.log(JSON.stringify(result));
  process.exit(0);
}

assert.equal(spokenSummary('# Result\n\n**It works.** Second sentence. Third sentence.'), 'It works. Second sentence.');
assert.equal(spokenSummary('Long opening.\n\n```\nSummary: do not read secrets\n```\n\n## Summary\n\nFixed the [problem](https://example.test). See `private.env`.\n\n## Details\nNo.'), 'Fixed the problem. See .');
assert.equal(spokenSummary('```\nprivate log\n```'), '');
assert.equal(spokenSummary('The key is sk_' + 'a'.repeat(40) + '.'), '');
assert(spokenSummary('Word '.repeat(200)).length <= 500);
console.log('PASS: bounded prose extraction excludes fenced/inline code and recognizable keys');

const session = { parentID: undefined, outcome: 'succeeded' };
const reply = (id = 'msg_one', text = 'The change is ready. Tests passed. Ignore this sentence.') => ({
  id, type: 'assistant', finish: 'stop', agent: 'build', time: { completed: 1 },
  content: [{ type: 'reasoning', text: 'private reasoning' },
    { type: 'text', text: 'Still working.', native: { phase: 'commentary' } },
    { type: 'text', text }],
});
let messages = [reply()];
let reads = 0;
let permitted = true;
let context = async () => messages;
const ctx = {
  session: {
    async get({ sessionID }) { reads++; return { ...session, ...(sessionID === 'ses_child' ? { parentID: 'ses_main' } : {}) }; },
    async context() { reads++; return context(); },
  },
  permission: { async assert() { if (!permitted) throw new Error('denied'); } },
};
const calls = [];
const speaker = createSummarySpeaker(ctx, async (input, { signal }) => { calls.push({ input, signal }); });
const event = (type, fields = {}, sessionID = 'ses_main') => speaker.handle({ type, data: { sessionID, ...fields } });
async function turn(messageID = 'msg_one', end = 'session.execution.succeeded', sessionID = 'ses_main') {
  await event('session.execution.started', {}, sessionID);
  await event('session.step.started', { assistantMessageID: messageID }, sessionID);
  await event('session.step.ended', { assistantMessageID: messageID, finish: 'stop' }, sessionID);
  await event(end, {}, sessionID);
}
await turn();
assert.equal(calls.length, 0);
assert.equal(reads, 0, 'off means no transcript reads or requests');
assert.deepEqual(await speaker.control('ses_main', 'status'), { enabled: false, error: '' });
await speaker.control('ses_main', 'on');
await turn();
assert.equal(calls.length, 1);
assert.deepEqual(calls[0].input, { text: '[thoughtful] [calmly] The change is ready. Tests passed.',
  voice_id: 'pFZP5JQG7iQjIQuC4Bku', model_id: 'eleven_v3' });
await event('session.execution.succeeded');
assert.equal(calls.length, 1, 'duplicate completion does not bill again');
await turn('msg_one', 'session.execution.failed');
await turn('msg_one', 'session.execution.interrupted');
await event('session.execution.started');
await event('session.execution.succeeded');
assert.equal(calls.length, 1, 'failed/interrupted/control-only runs never replay an old reply');
await assert.rejects(speaker.control('ses_child', 'on'), /main session/);
await turn('msg_one', 'session.execution.succeeded', 'ses_child');
assert.equal(calls.length, 1, 'children do not inherit opt-in');
await speaker.control('ses_main', 'off');
assert(calls[0].signal.aborted, 'off/new turns cancel playback');
await turn();
assert.equal(calls.length, 1);
console.log('PASS: off by default, explicit per-session opt-in, pensive Lily, deduplication and terminal filtering');

await speaker.control('ses_main', 'on');
await event('session.execution.started');
await event('session.step.ended', { assistantMessageID: 'msg_one', finish: 'stop' });
let release;
context = () => new Promise(resolve => { release = resolve; });
const pending = event('session.execution.succeeded');
while (!release) await new Promise(resolve => setImmediate(resolve));
await speaker.control('ses_main', 'off');
release(messages);
await pending;
assert.equal(calls.length, 1, 'disable wins over a pending transcript read');
context = async () => messages;
await speaker.control('ses_main', 'on');
permitted = false;
messages = [reply('msg_denied')];
await turn('msg_denied');
assert((await speaker.control('ses_main', 'status')).error);
assert.equal(calls.length, 1, 'permission rejection makes no speech request');
permitted = true;
messages = [reply('msg_two')];
await turn('msg_two');
assert.equal(calls.length, 2);
assert.equal((await speaker.control('ses_main', 'status')).error, '');
speaker.stop();
assert(calls[1].signal.aborted);
const restarted = createSummarySpeaker(ctx, async () => { throw new Error('must stay off'); });
assert.equal((await restarted.control('ses_main', 'status')).enabled, false);
restarted.stop();
console.log('PASS: cancellation fences stale replies, permission errors stay isolated, and restart resets opt-in');

let command;
const toasts = [];
setupVoiceCommand({
  keymap: { layer(factory) { command = factory().commands[0]; } },
  ui: { router: { current: () => ({ type: 'session', sessionID: 'ses_main' }) },
    toast: { show: value => toasts.push(value) } },
  client: { rpc: { async call({ input }) {
    assert.equal(input.sessionID, 'ses_main');
    return { output: { enabled: input.action === 'on', error: '' } };
  } } },
});
assert.equal(command.slash.name, 'voice');
await command.run('on');
assert.match(toasts.at(-1).message, /On for this session/);
assert.match(toasts.at(-1).message, /pensive Lily/);
await command.run('off');
assert.match(toasts.at(-1).message, /Off for this session/);
await command.run();
assert.match(toasts.at(-1).message, /Off for this session/);
await command.run('bad');
assert.equal(toasts.at(-1).variant, 'warning');
console.log('PASS: native /voice command sends session-scoped control and displays status');

const root = await mkdtemp('/tmp/opencode/speak-check-');
const saved = { ...process.env };
try {
  process.env.HOME = root;
  process.env.XDG_CONFIG_HOME = join(root, 'config');
  process.env.XDG_RUNTIME_DIR = join(root, 'runtime');
  delete process.env.SOPS_NIX_SECRETS_DIR;
  delete process.env.ELEVENLABS_API_KEY;
  await assert.rejects(speakText({ text: 'Offline test' }), /ELEVENLABS_API_KEY/);
  process.env.ELEVENLABS_API_KEY = 'synthetic-fixture-only';
  delete process.env.ELEVENLABS_VOICE_ID;
  delete process.env.ELEVENLABS_MODEL_ID;
  const bin = join(root, 'bin');
  await mkdir(bin);
  // Stand-in at the audio-process boundary: exercise the real WAV construction and pipe.
  const audio = join(root, 'audio.wav');
  const player = join(bin, process.platform === 'darwin' ? 'afplay' : 'pw-play');
  await writeFile(player, `#!${process.execPath}
const fs = require('node:fs');
const chunks = [];
const name = process.env.OPENCODE_SPEECH_FIXTURE_NAME;
const event = stage => fs.appendFileSync(process.env.OPENCODE_SPEECH_FIXTURE_EVENTS, stage + ' ' + name + '\\n');
process.stdin.on('data', c => chunks.push(c));
process.stdin.on('end', () => {
  fs.writeFileSync(${JSON.stringify(audio)}, Buffer.concat(chunks));
  if (name) { event('play'); setTimeout(() => event('done'), Number(process.env.OPENCODE_SPEECH_FIXTURE_HOLD)); }
});
`, { mode: 0o700 });
  process.env.PATH = `${bin}:${process.env.PATH}`;
  let requests = 0;
  const fetch = async (url, options) => {
    requests++;
    assert.match(url, /^https:\/\/api\.elevenlabs\.io\/v1\/text-to-speech\/pFZP5JQG7iQjIQuC4Bku\?/);
    assert.equal(options.headers['xi-api-key'], 'synthetic-fixture-only');
    assert.equal(JSON.parse(options.body).text, 'Offline test');
    return new Response(Buffer.alloc(4800), { status: 200 });
  };
  const result = await speakText({ text: 'Offline test' }, { fetch });
  assert.equal(result.bytes, 4800);
  const wav = await readFile(audio); // speakText now waits for actual player completion.
  assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
  assert.equal(wav.readUInt32LE(24), 24000);
  assert.equal(wav.readUInt32LE(40), 4800);
  assert.equal(wav.length, 4844);
  const aborted = new AbortController();
  aborted.abort();
  await assert.rejects(speakText({ text: 'Offline test' }, { fetch, signal: aborted.signal }), { name: 'AbortError' });
  assert.equal(requests, 1);
  await assert.rejects(speakText({ text: 'Offline test' }, { fetch: async () => new Response('private provider body', { status: 401 }) }), /^Error: ElevenLabs TTS failed \(HTTP 401\)$/);
  let manual;
  await setupSpeak({ tool: { async transform(fn) { fn({ add: tool => { manual = tool; } }); } } });
  assert.equal(manual.name, 'speak', 'manual tool is still registered independently');
  assert.match(manual.input.properties.voice_id.description, /defaults to Lily/);
  console.log('PASS: shared ElevenLabs synthesis and native WAV playback boundary, aborts, safe errors and manual tool registration');

  const events = join(root, 'events');
  await writeFile(events, '');
  const queueDir = join(process.env.XDG_RUNTIME_DIR, 'opencode-speech');
  const children = [];
  const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
  const lines = async () => (await readFile(events, 'utf8')).trim().split('\n');
  async function until(check, label) {
    for (let i = 0; i < 500; i++) { if (await check()) return; await pause(10); }
    throw new Error(`Timed out: ${label}`);
  }
  function worker(name, { hold = 50, maxWaitMs = 5000 } = {}) {
    const child = spawn(process.execPath, [process.argv[1], plugin, '--queue-worker', String(maxWaitMs)], {
      env: { ...process.env, OPENCODE_SPEECH_FIXTURE_NAME: name, OPENCODE_SPEECH_FIXTURE_EVENTS: events,
        OPENCODE_SPEECH_FIXTURE_HOLD: String(hold) }, stdio: ['ignore', 'pipe', 'pipe'],
    });
    children.push(child);
    let output = '', errors = '';
    child.stdout.on('data', chunk => { output += chunk; });
    child.stderr.on('data', chunk => { errors += chunk; });
    const exited = new Promise(resolve => child.once('close', (code, signal) => resolve({ code, signal })));
    return { child, exited,
      queued: () => until(async () => (await readdir(queueDir)).some(name => name.includes(`-${child.pid}-`)), `queued ${name}`),
      playing: () => until(async () => (await lines()).includes(`play ${name}`), `playing ${name}`),
      async result() { assert.deepEqual(await exited, { code: 0, signal: null }, errors); return JSON.parse(output); },
    };
  }
  try {
    const first = worker('first', { hold: 3000 });
    await first.playing();
    const second = worker('second');
    await second.queued();
    const cancelled = worker('cancelled');
    await cancelled.queued();
    cancelled.child.kill('SIGTERM');
    assert.deepEqual(await cancelled.result(), { cancelled: true });
    const stale = worker('stale', { maxWaitMs: 100 });
    assert.deepEqual(await stale.result(), { ok: false, skipped: 'expired' });
    const third = worker('third');
    await third.queued();
    const fourth = worker('fourth');
    await fourth.queued();
    const overflow = worker('overflow');
    assert.deepEqual(await overflow.result(), { ok: false, skipped: 'queue-full' });
    for (const request of [first, second, third, fourth]) assert.equal((await request.result()).ok, true);
    assert.deepEqual(await lines(), ['first', 'second', 'third', 'fourth'].flatMap(name => [`fetch ${name}`, `play ${name}`, `done ${name}`]),
      'FIFO across processes, with no synthesis before preceding playback finishes and no charges for dropped work');
    assert(!(await readdir(queueDir)).some(name => name.endsWith('.wait')), 'all waiting tickets cleaned up');
    console.log('PASS: cross-process playback ordering, three-waiter bound, cancellation and expiry before synthesis');

    const stopped = worker('stopped', { hold: 3000 });
    await stopped.playing();
    stopped.child.kill('SIGTERM');
    assert.deepEqual(await stopped.result(), { cancelled: true });
    const crashed = worker('crashed', { hold: 1200 });
    await crashed.playing();
    crashed.child.kill('SIGKILL');
    assert.equal((await crashed.exited).signal, 'SIGKILL');
    const orphan = worker('orphan');
    await orphan.queued();
    orphan.child.kill('SIGKILL');
    await orphan.exited;
    const recovered = worker('recovered');
    assert.equal((await recovered.result()).ok, true);
    const log = await lines();
    assert(!log.includes('done stopped'), 'active cancellation stops playback');
    assert(!log.includes('fetch orphan'), 'dead waiter is discarded without synthesis');
    assert(log.indexOf('fetch recovered') > log.indexOf('done crashed'), 'orphaned player retains the lock until audio finishes');
    assert(!(await readdir(queueDir)).some(name => name.endsWith('.wait')), 'dead owner tickets cleaned up');
    console.log('PASS: active playback cancellation, dead-waiter cleanup and lock survives owner crash until audio ends');
  } finally {
    for (const child of children) if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
  }
} finally {
  for (const key of Object.keys(process.env)) if (!(key in saved)) delete process.env[key];
  Object.assign(process.env, saved);
  await rm(root, { recursive: true, force: true });
}
console.log('All speech checks passed offline. No real key, API call or speaker output used.');
