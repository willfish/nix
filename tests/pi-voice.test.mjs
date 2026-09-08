import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import test from 'node:test';

const extensionPath = new URL('../home/config/voice/pi_voice.mjs', import.meta.url);
test('Pi voice extension is available', () => {
  assert.ok(existsSync(extensionPath), 'missing Pi voice extension');
});

if (existsSync(extensionPath)) {
  const { registerVoice } = await import(extensionPath);
  const { mkdtemp, rm, stat, writeFile, readFile } = await import('node:fs/promises');
  const { tmpdir } = await import('node:os');
  const { join } = await import('node:path');
  const { default: net } = await import('node:net');

  async function fixture(t, response = () => ({ ok: true })) {
    const directory = await mkdtemp(join(tmpdir(), 'pi-voice-'));
    const events = [];
    const controlPath = join(directory, 'control.sock');
    const path = join(directory, 'pi.sock');
    const control = net.createServer((socket) => {
      let data = '';
      socket.on('data', (chunk) => {
        data += chunk;
        if (data.includes('\n')) {
          events.push(JSON.parse(data));
          socket.end(`${JSON.stringify(response(events.at(-1), events.length))}\n`);
        }
      });
    });
    await new Promise((resolve) => control.listen(controlPath, resolve));
    const hooks = {};
    const sent = [];
    let editor = '';
    let idle = true;
    let pending = false;
    let session = 'session-a';
    const pi = { on: (name, callback) => { hooks[name] = callback; },
      sendUserMessage: (text) => { sent.push(text); idle = false; },
      getSessionName: () => 'Voice test' };
    const ctx = { hasUI: true, cwd: directory, isIdle: () => idle,
      hasPendingMessages: () => pending,
      sessionManager: { getSessionId: () => session, getLeafId: () => 'turn-a' },
      ui: { getEditorText: () => editor, setEditorText: (value) => { editor = value; }, notify: () => {} } };
    registerVoice(pi, { AGENT_VOICE_TOKEN: 'token', AGENT_VOICE_KIND: 'pi',
      AGENT_VOICE_ADAPTER_SOCKET: path, AGENT_VOICE_SOCKET: controlPath,
      AGENT_VOICE_LAUNCH_PID: String(process.ppid) });
    t.after(async () => {
      await hooks.session_shutdown({}, ctx);
      await new Promise((resolve) => control.close(resolve));
      await rm(directory, { recursive: true, force: true });
    });
    await hooks.session_start({}, ctx);
    const request = (command, fields = {}) => new Promise((resolve, reject) => {
      const socket = net.createConnection(path);
      let data = '';
      socket.on('error', reject);
      socket.on('connect', () => socket.write(`${JSON.stringify({ command, token: 'token', session: 'session-a', ...fields })}\n`));
      socket.on('data', (chunk) => { data += chunk; });
      socket.on('end', () => resolve(JSON.parse(data)));
    });
    return { request, events, hooks, ctx, sent, path, pi,
      editor: () => editor, setEditor: (value) => { editor = value; },
      setIdle: (value) => { idle = value; },
      setPending: (value) => { pending = value; },
      setSession: (value) => { session = value; } };
  }

  test('session startup retries until its launcher registration is accepted', async (t) => {
    const f = await fixture(t, (_event, count) => ({ ok: true, accepted: count >= 4 }));
    assert.equal(f.events.filter((event) => event.event.type === 'session').length, 4);
  });

  test('session startup stops retrying an unknown token within two seconds', async (t) => {
    const start = performance.now();
    const f = await fixture(t, () => ({ ok: true, accepted: false }));
    const elapsed = performance.now() - start;
    assert.ok(f.events.length > 1);
    assert.ok(elapsed < 2200, `startup took ${elapsed} ms`);
    assert.equal((await f.request('status')).ok, true);
  });

  test('stages speech alongside typed text and submits it once', async (t) => {
    const f = await fixture(t);
    f.setEditor('Please');
    assert.equal((await f.request('stage', { text: 'explain this' })).ok, true);
    assert.equal(f.editor(), 'Please explain this');
    assert.deepEqual(f.sent, []);
    assert.equal((await f.request('submit')).ok, true);
    assert.deepEqual(f.sent, ['Please explain this']);
    f.setIdle(true);
    assert.equal((await f.request('submit')).ok, false);
    assert.equal((await stat(f.path)).mode & 0o777, 0o600);
  });

  test('rejects wrong session, token, busy and pending input', async (t) => {
    const f = await fixture(t);
    for (const fields of [{ session: 'other' }, { token: 'wrong' }]) {
      assert.equal((await f.request('stage', { text: 'hello', ...fields })).ok, false);
    }
    f.setIdle(false);
    assert.equal((await f.request('stage', { text: 'hello' })).ok, false);
    f.setIdle(true);
    f.setPending(true);
    assert.equal((await f.request('stage', { text: 'hello' })).ok, false);
    assert.equal(f.editor(), '');
  });

  test('manual submission or editing disarms voice Send', async (t) => {
    const f = await fixture(t);
    await f.request('stage', { text: 'hello' });
    f.hooks.input({ source: 'interactive' }, f.ctx);
    assert.equal((await f.request('submit')).ok, false);
    await f.request('stage', { text: 'world' });
    f.setEditor('changed');
    assert.equal((await f.request('submit')).ok, false);
    assert.deepEqual(f.sent, []);
  });

  test('explicit hotkey submit sends the edited voice draft once', async (t) => {
    const f = await fixture(t);
    await f.request('stage', { text: 'Original dictation' });
    f.setEditor('My corrected dictation');
    assert.equal((await f.request('submit', { allow_edited: true })).ok, true);
    assert.deepEqual(f.sent, ['My corrected dictation']);
    assert.equal(f.editor(), '');
    f.setIdle(true);
    assert.equal((await f.request('submit', { allow_edited: true })).ok, false);
    assert.deepEqual(f.sent, ['My corrected dictation']);
  });

  test('only literal true opts into submitting an edited voice draft', async (t) => {
    const f = await fixture(t);
    await f.request('stage', { text: 'Original dictation' });
    f.setEditor('My corrected dictation');
    assert.equal((await f.request('submit', { allow_edited: 'true' })).ok, false);
    assert.deepEqual(f.sent, []);
    assert.equal(f.editor(), 'My corrected dictation');
  });

  test('explicit hotkey submit rejects empty or unsafe edited drafts', async (t) => {
    const f = await fixture(t);
    for (const invalid of ['', '   ', '\x1b[1;1H']) {
      f.setEditor('');
      await f.request('stage', { text: 'Original dictation' });
      f.setEditor(invalid);
      assert.equal((await f.request('submit', { allow_edited: true })).ok, false);
      assert.equal(f.editor(), invalid);
    }
    assert.deepEqual(f.sent, []);
  });

  test('explicit hotkey submit never sends an unarmed hand typed prompt', async (t) => {
    const f = await fixture(t);
    f.setEditor('Unrelated hand typed prompt');
    assert.equal((await f.request('submit', { allow_edited: true })).ok, false);
    assert.deepEqual(f.sent, []);
    assert.equal(f.editor(), 'Unrelated hand typed prompt');
  });

  test('only speaks a final successful assistant reply after Pi settles', async (t) => {
    const f = await fixture(t);
    f.setIdle(false);
    await f.hooks.agent_start({}, f.ctx);
    f.hooks.agent_end({ messages: [{ role: 'assistant', stopReason: 'stop', content: [{ type: 'thinking', thinking: 'private' }, { type: 'text', text: 'Hello.' }] }] }, f.ctx);
    await f.hooks.agent_settled({}, f.ctx);
    assert.equal(f.events.filter((event) => event.event.type === 'reply').length, 0);
    f.setIdle(true);
    await f.hooks.agent_settled({}, f.ctx);
    await f.hooks.agent_settled({}, f.ctx);
    assert.deepEqual(f.events.filter((event) => event.event.type === 'reply').map((event) => event.event.text), ['Hello.']);
    assert.equal(f.events[0].event.session, 'session-a');
    assert.equal(f.events[0].event.pid, process.pid);
  });

  test('blocking extension UI rejects recording delivery', async (t) => {
    const f = await fixture(t);
    await f.hooks.ui_prompt_start({}, f.ctx);
    assert.equal((await f.request('status')).result.state, 'blocked');
    assert.equal((await f.request('stage', { text: 'hello' })).ok, false);
    await f.hooks.ui_prompt_end({}, f.ctx);
    assert.equal((await f.request('stage', { text: 'hello' })).ok, true);
  });

  test('Unicode dictation survives socket packet boundaries', async (t) => {
    const f = await fixture(t);
    const wire = Buffer.from(`${JSON.stringify({ command: 'stage', token: 'token', session: 'session-a', text: 'café' })}\n`);
    const split = wire.indexOf(Buffer.from('é')) + 1;
    const response = await new Promise((resolve, reject) => {
      const socket = net.createConnection(f.path);
      let data = '';
      socket.on('error', reject);
      socket.on('connect', () => {
        socket.write(wire.subarray(0, split));
        setTimeout(() => socket.write(wire.subarray(split)), 10);
      });
      socket.on('data', (chunk) => { data += chunk; });
      socket.on('end', () => resolve(JSON.parse(data)));
    });
    assert.equal(response.ok, true);
    assert.equal(f.editor(), 'café');
  });

  test('session replacement rejects the previously selected session', async (t) => {
    const f = await fixture(t);
    await f.request('stage', { text: 'old draft' });
    await f.hooks.session_shutdown({}, f.ctx);
    f.setSession('session-b');
    await f.hooks.session_start({ reason: 'new' }, f.ctx);
    assert.equal((await f.request('stage', { text: 'wrong session' })).ok, false);
    const rebound = await f.request('status', { session: 'session-b' });
    assert.equal(rebound.result.draft, false);
  });

  test('an aborted final run cannot speak an earlier successful candidate', async (t) => {
    const f = await fixture(t);
    f.hooks.agent_end({ messages: [{ role: 'assistant', stopReason: 'stop', content: [{ type: 'text', text: 'Stale.' }] }] }, f.ctx);
    f.hooks.agent_end({ messages: [{ role: 'assistant', stopReason: 'aborted', content: [{ type: 'text', text: 'Partial.' }] }] }, f.ctx);
    await f.hooks.agent_settled({}, f.ctx);
    assert.equal(f.events.filter((event) => event.event.type === 'reply').length, 0);
  });

  test('socket conflicts disable voice without breaking Pi or removing another file', async (t) => {
    const directory = await mkdtemp(join(tmpdir(), 'pi-voice-conflict-'));
    t.after(() => rm(directory, { recursive: true, force: true }));
    const path = join(directory, 'pi.sock');
    await writeFile(path, 'existing');
    const hooks = {};
    const notices = [];
    registerVoice({ on: (name, fn) => { hooks[name] = fn; } }, {
      AGENT_VOICE_TOKEN: 'token', AGENT_VOICE_KIND: 'pi',
      AGENT_VOICE_ADAPTER_SOCKET: path, AGENT_VOICE_SOCKET: '/missing',
      AGENT_VOICE_LAUNCH_PID: String(process.ppid),
    });
    const ctx = { hasUI: true, sessionManager: { getSessionId: () => 'a' },
      ui: { notify: (text) => notices.push(text) } };
    await assert.doesNotReject(hooks.session_start({}, ctx));
    assert.equal(await readFile(path, 'utf8'), 'existing');
    assert.equal(notices.length, 1);
  });

  test('plain Pi or inherited child environment does not enable voice', () => {
    const hooks = [];
    const pi = { on: (name) => hooks.push(name) };
    registerVoice(pi, {});
    registerVoice(pi, { AGENT_VOICE_TOKEN: 'token', AGENT_VOICE_KIND: 'pi',
      AGENT_VOICE_ADAPTER_SOCKET: '/tmp/unused', AGENT_VOICE_SOCKET: '/tmp/unused',
      AGENT_VOICE_LAUNCH_PID: String(process.pid) });
    assert.deepEqual(hooks, []);
  });
}
