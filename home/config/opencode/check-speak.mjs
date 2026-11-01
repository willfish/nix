// Manual offline regressions for opt-in speech; no real credentials or speech API.
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const plugin = resolve(process.argv[2] || 'home/config/opencode/harness');
const { spokenSummary, createSummarySpeaker, setupVoiceCommand } = await import(pathToFileURL(join(plugin, 'auto-speak.ts')));
const { speakText, setupSpeak } = await import(pathToFileURL(join(plugin, 'speak.ts')));

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
  await writeFile(player, `#!${process.execPath}\nconst fs = require('node:fs'); const chunks = []; process.stdin.on('data', c => chunks.push(c)); process.stdin.on('end', () => fs.writeFileSync(${JSON.stringify(audio)}, Buffer.concat(chunks)));\n`, { mode: 0o700 });
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
  let wav;
  for (let i = 0; i < 100; i++) {
    try { wav = await readFile(audio); break; } catch { await new Promise(resolve => setTimeout(resolve, 10)); }
  }
  assert(wav, 'audio process received the buffer');
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
} finally {
  for (const key of Object.keys(process.env)) if (!(key in saved)) delete process.env[key];
  Object.assign(process.env, saved);
  await rm(root, { recursive: true, force: true });
}
console.log('All speech checks passed offline. No real key, API call or speaker output used.');
