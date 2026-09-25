import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import test from 'node:test';

const extensionPath = new URL('../home/config/pi/extensions/pi-voice.ts', import.meta.url);
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
    const shortcuts = {};
    const statuses = [];
    const pi = { on: (name, callback) => { hooks[name] = callback; },
      sendUserMessage: (text) => { sent.push(text); idle = false; },
      getSessionName: () => 'Voice test',
      registerShortcut: (key, options) => { shortcuts[key] = options; } };
    const ctx = { hasUI: true, cwd: directory, isIdle: () => idle,
      hasPendingMessages: () => pending,
      sessionManager: { getSessionId: () => session, getLeafId: () => 'turn-a' },
      ui: { getEditorText: () => editor, setEditorText: (value) => { editor = value; }, notify: () => {},
        setStatus: (id, message) => { statuses.push([id, message]); } } };
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
    return { request, events, hooks, ctx, sent, path, pi, shortcuts, statuses,
      editor: () => editor, setEditor: (value) => { editor = value; },
      setIdle: (value) => { idle = value; },
      setPending: (value) => { pending = value; },
      setSession: (value) => { session = value; } };
  }

  test('Alt+M toggles controller dictation without a terminal meter', async (t) => {
    const f = await fixture(t, (event) => {
      if (event.action === 'dictate' || event.action === 'status')
        return { ok: true, phase: 'recording', input_level: 0.4 };
      return { ok: true, accepted: true, attached: { state: 'ready', token: 'token' } };
    });
    assert.ok(f.shortcuts['alt+m']);
    assert.ok(f.shortcuts['alt+n']);
    await f.shortcuts['alt+m'].handler(f.ctx);
    assert.ok(f.events.some((event) => event.action === 'dictate' && event.token === 'token'));
    assert.equal(f.statuses.filter((entry) => entry[0] === 'voice').length, 0);
    await f.shortcuts['alt+n'].handler(f.ctx);
    assert.ok(f.events.some((event) => event.action === 'dictate-cancel'));
  });

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

  const flush = () => new Promise(resolve => setImmediate(resolve));
  async function until(predicate) {
    for (let i = 0; i < 200; i++) { if (predicate()) return; await flush(); }
    assert.ok(predicate(), 'background operation did not reach checkpoint');
  }
  function clock() {
    const timers = new Set();
    let now = 0;
    return { timers, get now() { return now; }, advance(ms) { now += ms; },
      setTimeout(fn, delay) {
        const timer = { fn, delay, unrefed: false, unref() { this.unrefed = true; } };
        timers.add(timer);
        return timer;
      },
      clearTimeout(timer) { timers.delete(timer); },
      async next() {
        const timer = [...timers][0];
        assert.ok(timer, 'expected a scheduled retry/heartbeat');
        assert.equal(timer.unrefed, true);
        timers.delete(timer);
        now += timer.delay;
        timer.fn();
        await flush();
      },
    };
  }
  async function managed(t, { respond, mode = 'tui', env = {}, prepare, register = registerVoice } = {}) {
    const directory = await mkdtemp(join(tmpdir(), 'piv-'));
    const runtime = join(directory, 'pi-voice');
    if (prepare) await prepare(directory, runtime);
    const scheduler = clock(), requests = [], hooks = {}, notices = [];
    let editor = '', idle = true, pending = false, session = 'managed-a';
    const sent = [];
    const ctx = { mode, hasUI: !['print', 'json'].includes(mode), cwd: directory,
      model: { id: 'local-model' }, thinkingLevel: 'medium',
      isIdle: () => idle, hasPendingMessages: () => pending,
      sessionManager: { getSessionId: () => session, getLeafId: () => 'leaf' },
      ui: { notify: text => notices.push(text), getEditorText: () => editor,
        setEditorText: text => { editor = text; } } };
    register({ on: (name, fn) => { hooks[name] = fn; }, sendUserMessage: text => sent.push(text) }, {
      HERDR_ENV: '1', HERDR_SOCKET_PATH: '/fixture/herdr.sock', HERDR_PANE_ID: 'w1:p1',
      XDG_RUNTIME_DIR: directory, ...env,
    }, { ...scheduler, now: () => scheduler.now, random: () => 0, exchange: async (path, request, timeout, signal) => {
      assert.equal(path, join(runtime, 'control.sock'));
      assert.equal(timeout, 1000);
      requests.push(request);
      if (respond) return respond(request, signal, requests.length);
      return request.action === 'attach' ? { ok: true, attached: { token: 'managed-token', state: 'connecting' } }
        : { ok: true, accepted: true };
    } });
    t.after(async () => {
      await hooks.session_shutdown?.({}, ctx);
      await flush();
      await rm(directory, { recursive: true, force: true });
    });
    const start = hooks.session_start({}, ctx);
    await start;
    const target = () => requests.find(request => request.action === 'attach')?.target;
    const request = (command, fields = {}) => new Promise((resolve, reject) => {
      const socket = net.createConnection(target().adapter_socket);
      socket.setEncoding('utf8');
      let data = '';
      socket.on('error', reject);
      socket.on('connect', () => socket.write(JSON.stringify({ ...target(), command, token: 'managed-token', ...fields }) + '\n'));
      socket.on('data', chunk => { data += chunk; });
      socket.on('end', () => resolve(JSON.parse(data)));
    });
    return { scheduler, requests, hooks, ctx, target, request, directory, runtime, notices, sent,
      editor: () => editor, setIdle: value => { idle = value; },
      setPending: value => { pending = value; }, setSession: value => { session = value; } };
  }

  for (const child of [false, true]) {
    test(`managed ${child ? 'child stays silent' : 'root emits reply'} and accepts dictation`, async t => {
      const f = await managed(t, { env: { PI_TEAM_CHILD: child ? '1' : '0' } });
      await until(() => f.scheduler.timers.size);
      f.hooks.agent_end({ messages: [{ role: 'assistant', stopReason: 'stop',
        content: [{ type: 'text', text: '## Summary\nCompleted.' }] }] }, f.ctx);
      await f.hooks.agent_settled({}, f.ctx);
      await tick(f);
      await tick(f);
      const replies = f.requests.filter(request => request.event?.type === 'reply');
      assert.equal(replies.length, child ? 0 : 1);
      assert.equal((await f.request('stage', { text: 'Dictated text' })).ok, true);
      assert.equal(f.editor(), 'Dictated text');
    });
  }

  test('managed TUI binds a private UUID endpoint and acknowledges operational readiness', async t => {
    const f = await managed(t, { env: { PI_TEAM_CHILD: '1', PI_VOICE_HARNESS: 'qwen-pi' } });
    await until(() => f.scheduler.timers.size);
    const target = f.target();
    assert.deepEqual(Object.keys(target).sort(), ['pane', 'socket', 'pid', 'harness', 'adapter_socket', 'session',
      'bridge_id', 'activation', 'team_child', 'model', 'thinking'].sort());
    assert.equal(target.team_child, true);
    assert.equal(target.harness, 'qwen-pi');
    assert.equal(target.model, 'local-model');
    assert.equal(target.thinking, 'medium');
    assert.match(target.bridge_id, /^[0-9a-f-]{36}$/);
    assert.ok(target.activation > 0);
    assert.equal(target.adapter_socket, join(f.runtime, `pi-${process.pid}-${target.bridge_id}.sock`));
    assert.equal((await stat(f.runtime)).mode & 0o777, 0o700);
    assert.equal((await stat(target.adapter_socket)).mode & 0o777, 0o600);
    assert.equal(f.requests[1].event.type, 'ready');
    assert.equal(f.requests[1].token, 'managed-token');
    const status = (await f.request('status')).result;
    assert.equal(status.bridge_id, target.bridge_id);
    assert.equal(status.activation, target.activation);
    assert.equal(status.ready, true);
    assert.deepEqual(f.notices, []);
    // Each request naturally reaches EOF without removing the registration.
    await f.request('status');
    await f.request('stage', { text: 'hello' });
    assert.equal(f.requests.some(request => request.action === 'detach'), false);
    await f.scheduler.next();
    await until(() => f.requests.some(request => request.event?.type === 'heartbeat'));
    assert.equal(f.scheduler.now, 5000);
    await f.hooks.session_shutdown({}, f.ctx);
    await flush();
    const detached = f.requests.find(request => request.action === 'detach');
    assert.deepEqual(detached, { action: 'detach', token: 'managed-token', pid: process.pid,
      bridge_id: target.bridge_id, activation: target.activation });
    assert.equal(existsSync(target.adapter_socket), false);
  });

  test('managed startup and UI hooks never await an unresolved attach exchange', async t => {
    let resolveAttach;
    const f = await managed(t, { respond: request => request.action === 'attach'
      ? new Promise(resolve => { resolveAttach = resolve; }) : { ok: true, accepted: true } });
    await until(() => resolveAttach);
    for (const name of ['model_select', 'thinking_level_select', 'agent_start', 'ui_prompt_start']) {
      await f.hooks[name]({}, f.ctx);
    }
    f.setIdle(false);
    resolveAttach({ ok: true, attached: { token: 'managed-token', state: 'connecting' } });
    await until(() => f.requests.some(request => request.event?.type === 'ready'));
    const event = f.requests.find(request => request.event?.type === 'ready').event;
    assert.equal(event.ready, false);
    assert.equal(event.state, 'blocked');
    assert.equal((await f.request('stage', { text: 'never bypass a question' })).ok, false);
    await f.hooks.ui_prompt_end({}, f.ctx);
    f.setIdle(true);
    assert.equal((await f.request('stage', { text: 'after question' })).ok, true);
  });

  test('managed retries continue beyond sixty seconds and recover after a lost response', async t => {
    let available = false, dropped = false;
    const f = await managed(t, { respond: request => {
      if (request.action !== 'attach') return { ok: true, accepted: true };
      if (!available) throw new Error('controller absent');
      if (!dropped) { dropped = true; throw new Error('admitted but response dropped'); }
      return { ok: true, attached: { token: 'managed-token', state: 'connecting' } };
    } });
    await until(() => f.scheduler.timers.size);
    while (f.scheduler.now <= 70000) { await f.scheduler.next(); await until(() => f.scheduler.timers.size); }
    available = true;
    await f.scheduler.next();
    await until(() => f.scheduler.timers.size);
    await f.scheduler.next();
    await until(() => f.requests.some(request => request.event?.type === 'ready'));
    const targets = f.requests.filter(request => request.action === 'attach').map(request => request.target);
    assert.ok(targets.length > 15);
    for (const target of targets) assert.deepEqual(target, targets[0]);
    assert.equal((await f.request('status')).ok, true);
    assert.deepEqual(f.notices, []);
  });

  test('ready attach installs its token and a dropped ready acknowledgement preserves the draft', async t => {
    let readyCount = 0;
    const f = await managed(t, { respond: request => {
      if (request.action === 'attach') return { ok: true, attached: { token: 'managed-token', state: 'ready' } };
      if (request.event?.type === 'ready' && ++readyCount === 1) throw new Error('lost ready ack');
      return { ok: true, accepted: true };
    } });
    await until(() => f.scheduler.timers.size);
    assert.equal((await f.request('stage', { text: 'keep this draft' })).ok, true);
    await f.scheduler.next();
    await until(() => f.scheduler.timers.size);
    assert.equal((await f.request('status')).result.draft, true);
    assert.equal(f.editor(), 'keep this draft');
    assert.equal(f.requests.filter(request => request.action === 'attach').length, 1);
    assert.equal(readyCount, 2);
    assert.deepEqual(f.sent, []);
  });

  async function finalReply(f, text = 'Final reply.', turn = 'leaf') {
    f.ctx.sessionManager.getLeafId = () => turn;
    f.hooks.agent_end({ messages: [{ role: 'assistant', stopReason: 'stop',
      content: [{ type: 'text', text }] }] }, f.ctx);
    await f.hooks.agent_settled({}, f.ctx);
  }
  async function tick(f) {
    await f.scheduler.next();
    await until(() => f.scheduler.timers.size);
  }

  test('dropped reply survives transport loss, unknown token and reconnect without replaying input', async t => {
    let replies = 0, attaches = 0;
    const f = await managed(t, { respond: request => {
      if (request.action === 'attach') return { ok: true, attached: {
        token: ++attaches === 1 ? 'managed-token' : 'replacement-token', state: 'connecting' } };
      if (request.event?.type === 'reply') {
        if (++replies === 1) throw new Error('reply exchange dropped');
        if (replies === 2) return { ok: true, accepted: false };
      }
      return { ok: true, accepted: true };
    } });
    await until(() => f.scheduler.timers.size);
    await f.request('stage', { text: 'send once' });
    await f.request('submit');
    await f.request('stage', { text: 'keep editor only' });
    await finalReply(f);
    for (let i = 0; i < 7; i++) await tick(f);
    const sentReplies = f.requests.filter(request => request.event?.type === 'reply');
    assert.equal(sentReplies.length, 3);
    assert.deepEqual(sentReplies.map(request => request.event.text), Array(3).fill('Final reply.'));
    assert.equal(sentReplies.at(-1).token, 'replacement-token');
    assert.equal(attaches, 2);
    assert.deepEqual(f.sent, ['send once']);
    assert.equal(f.editor(), 'keep editor only');
    assert.equal((await f.request('submit', { token: 'replacement-token' })).ok, false);
  });

  for (const scenario of ['duplicate after dropped ack', 'unselected']) {
    test(`valid ${scenario} reply is acknowledged without reattachment or repeat delivery`, async t => {
      const seen = new Set(), delivered = [];
      let attempts = 0;
      const f = await managed(t, { respond: request => {
        if (request.action === 'attach') return { ok: true, attached: { token: 'managed-token', state: 'connecting' } };
        if (request.event?.type === 'reply') {
          attempts++;
          if (!seen.has(request.event.turn)) {
            seen.add(request.event.turn);
            if (scenario !== 'unselected') delivered.push(request.event.text);
          }
          if (scenario !== 'unselected' && attempts === 1) throw new Error('accepted but ack dropped');
        }
        return { ok: true, accepted: true };
      } });
      await until(() => f.scheduler.timers.size);
      await finalReply(f);
      for (let i = 0; i < 5; i++) await tick(f);
      assert.equal(attempts, scenario === 'unselected' ? 1 : 2);
      assert.deepEqual(delivered, scenario === 'unselected' ? [] : ['Final reply.']);
      assert.equal(f.requests.filter(request => request.action === 'attach').length, 1);
      assert.deepEqual(f.sent, []);
      assert.equal(f.editor(), '');
    });
  }

  test('an ok response without an explicit acknowledgement retains the reply', async t => {
    let replies = 0;
    const f = await managed(t, { respond: request => {
      if (request.action === 'attach') return { ok: true, attached: { token: 'managed-token', state: 'connecting' } };
      if (request.event?.type === 'reply' && ++replies === 1) return { ok: true };
      return { ok: true, accepted: true };
    } });
    await until(() => f.scheduler.timers.size);
    await finalReply(f);
    for (let i = 0; i < 4; i++) await tick(f);
    assert.equal(replies, 2);
    assert.equal(f.requests.filter(request => request.action === 'attach').length, 1);
  });

  test('a final reply queued before attachment is delivered only for the latest turn', async t => {
    let resolveAttach;
    const f = await managed(t, { respond: request => request.action === 'attach'
      ? new Promise(resolve => { resolveAttach = resolve; }) : { ok: true, accepted: true } });
    await until(() => resolveAttach);
    await finalReply(f, 'Old.', 'old');
    await f.hooks.agent_start({}, f.ctx);
    await finalReply(f, 'New.', 'new');
    resolveAttach({ ok: true, attached: { token: 'managed-token', state: 'ready' } });
    await until(() => f.scheduler.timers.size);
    for (let i = 0; i < 4; i++) await tick(f);
    assert.deepEqual(f.requests.filter(request => request.event?.type === 'reply')
      .map(request => request.event.text), ['New.']);
    assert.deepEqual(f.sent, []);
    assert.equal(f.editor(), '');
  });

  test('a late old reply acknowledgement cannot delete a newer turn reply', async t => {
    let finishReply;
    const f = await managed(t, { respond: request => {
      if (request.action === 'attach') return { ok: true, attached: { token: 'managed-token', state: 'connecting' } };
      if (request.event?.text === 'Old.') return new Promise(resolve => { finishReply = resolve; });
      return { ok: true, accepted: true };
    } });
    await until(() => f.scheduler.timers.size);
    await finalReply(f, 'Old.', 'old');
    await tick(f);
    await f.scheduler.next();
    await until(() => finishReply);
    await f.hooks.agent_start({}, f.ctx);
    await finalReply(f, 'New.', 'new');
    finishReply({ ok: true, accepted: true });
    await until(() => f.scheduler.timers.size);
    for (let i = 0; i < 4; i++) await tick(f);
    assert.deepEqual(f.requests.filter(request => request.event?.type === 'reply')
      .map(request => request.event.text), ['Old.', 'New.']);
  });

  for (const replacement of ['turn', 'session']) {
    test(`a newer ${replacement} invalidates a dropped final reply before retry`, async t => {
      const f = await managed(t, { respond: request => {
        if (request.action === 'attach') return { ok: true, attached: { token: 'managed-token', state: 'connecting' } };
        if (request.event?.type === 'reply') throw new Error('not delivered');
        return { ok: true, accepted: true };
      } });
      await until(() => f.scheduler.timers.size);
      await finalReply(f, 'Old.');
      await tick(f);
      await tick(f);
      if (replacement === 'turn') await f.hooks.agent_start({}, f.ctx);
      else {
        f.setSession('managed-b');
        await f.hooks.session_start({ reason: 'new' }, f.ctx);
        await until(() => f.scheduler.timers.size);
      }
      for (let i = 0; i < 3; i++) await tick(f);
      assert.equal(f.requests.filter(request => request.event?.type === 'reply').length, 1);
      assert.deepEqual(f.sent, []);
    });
  }

  test('unknown heartbeat token reattaches, resets armed Send and never replays input', async t => {
    let restarted = false;
    const f = await managed(t, { respond: request => {
      if (request.action === 'attach') return { ok: true, attached: { token: 'managed-token', state: 'connecting' } };
      if (request.event?.type === 'heartbeat' && !restarted) { restarted = true; return { ok: true, accepted: false }; }
      return { ok: true, accepted: true };
    } });
    await until(() => f.scheduler.timers.size);
    await f.request('stage', { text: 'retained editor only' });
    await f.scheduler.next();
    await until(() => f.scheduler.timers.size);
    assert.equal((await f.request('status')).ok, false);
    await f.scheduler.next();
    await until(() => f.requests.filter(request => request.event?.type === 'ready').length === 2);
    assert.equal((await f.request('status')).result.draft, false);
    assert.equal((await f.request('submit')).ok, false);
    assert.equal(f.editor(), 'retained editor only');
    assert.deepEqual(f.sent, []);
    assert.deepEqual(f.requests.filter(request => request.action === 'attach').map(request => request.target), [f.target(), f.target()]);
  });

  test('a new run during settled reporting suppresses the previous final reply', async t => {
    const f = await managed(t);
    await until(() => f.scheduler.timers.size);
    f.hooks.agent_end({ messages: [{ role: 'assistant', stopReason: 'stop', content: [{ type: 'text', text: 'Old reply.' }] }] }, f.ctx);
    const settled = f.hooks.agent_settled({}, f.ctx);
    f.setIdle(false);
    await f.hooks.agent_start({}, f.ctx);
    await settled;
    await f.scheduler.next();
    await until(() => f.scheduler.timers.size);
    await f.scheduler.next();
    assert.equal(f.requests.some(request => request.event?.type === 'reply'), false);
  });

  test('managed generation checks preserve busy, pending and main-pane question guards', async t => {
    const f = await managed(t);
    await until(() => f.scheduler.timers.size);
    for (const fields of [{ bridge_id: 'old' }, { activation: f.target().activation - 1 }, { pid: process.pid + 1 },
      { session: 'old' }, { token: 'old' }, { harness: 'unsupported' }, { bridge_id: undefined }, { activation: undefined }]) {
      assert.equal((await f.request('stage', { text: 'no', ...fields })).ok, false);
      assert.equal((await f.request('submit', fields)).ok, false);
    }
    f.setPending(true);
    assert.equal((await f.request('stage', { text: 'no' })).ok, false);
    f.setPending(false);
    await f.hooks.ui_prompt_start({ kind: 'select', title: 'Main-pane question' }, f.ctx);
    assert.equal((await f.request('status')).result.state, 'blocked');
    assert.equal((await f.request('stage', { text: 'no' })).ok, false);
    await f.hooks.ui_prompt_end({}, f.ctx);
    assert.equal((await f.request('stage', { text: 'yes' })).ok, true);
    assert.equal((await f.request('submit')).ok, true);
    assert.deepEqual(f.sent, ['yes']);
  });

  test('session activation changes identity and late attach cannot reopen a closed bridge', async t => {
    const replies = [];
    const f = await managed(t, { respond: request => request.action === 'attach'
      ? new Promise(resolve => replies.push(resolve)) : { ok: true, accepted: true } });
    await until(() => replies.length === 1);
    const first = f.target();
    await f.hooks.session_shutdown({}, f.ctx);
    f.setSession('managed-b');
    await f.hooks.session_start({ reason: 'new' }, f.ctx);
    await until(() => replies.length === 2);
    const second = f.requests.filter(request => request.action === 'attach')[1].target;
    assert.ok(second.activation > first.activation);
    assert.notEqual(second.bridge_id, first.bridge_id);
    assert.notEqual(second.adapter_socket, first.adapter_socket);
    replies[1]({ ok: true, attached: { token: 'new', state: 'connecting' } });
    await until(() => f.scheduler.timers.size);
    replies[0]({ ok: true, attached: { token: 'old', state: 'connecting' } });
    await flush();
    assert.equal(f.requests.some(request => request.token === 'old'), false);
    assert.equal(existsSync(first.adapter_socket), false);
    assert.equal(existsSync(second.adapter_socket), true);
    await f.hooks.session_shutdown({}, f.ctx);
    assert.equal(f.scheduler.timers.size, 0);
    assert.equal(existsSync(second.adapter_socket), false);
  });

  test('late heartbeat failure from an old activation cannot reset its replacement', async t => {
    let finishHeartbeat;
    const f = await managed(t, { respond: request => request.event?.type === 'heartbeat'
      ? new Promise(resolve => { finishHeartbeat = resolve; })
      : request.action === 'attach' ? { ok: true, attached: { token: 'managed-token', state: 'connecting' } }
        : { ok: true, accepted: true } });
    await until(() => f.scheduler.timers.size);
    await f.scheduler.next();
    await until(() => finishHeartbeat);
    await f.hooks.session_shutdown({}, f.ctx);
    await f.hooks.session_start({ reason: 'reload' }, f.ctx);
    await until(() => f.scheduler.timers.size);
    const attachedCount = f.requests.filter(request => request.action === 'attach').length;
    finishHeartbeat({ ok: true, accepted: false });
    await flush();
    assert.equal(f.requests.filter(request => request.action === 'attach').length, attachedCount);
    assert.equal(f.scheduler.timers.size, 1);
    assert.equal(f.requests.filter(request => request.event?.type === 'ready').length, 2);
  });

  test('shutdown aborts the active exchange and gives explicit detach its bounded attempt', async t => {
    let finishHeartbeat, finishDetach, heartbeatSignal;
    const f = await managed(t, { respond: (request, signal) => {
      if (request.action === 'attach') return { ok: true, attached: { token: 'managed-token', state: 'connecting' } };
      if (request.action === 'detach') return new Promise(resolve => { finishDetach = resolve; });
      if (request.event?.type === 'heartbeat') {
        heartbeatSignal = signal;
        return new Promise(resolve => { finishHeartbeat = resolve; });
      }
      return { ok: true, accepted: true };
    } });
    await until(() => f.scheduler.timers.size);
    await f.scheduler.next();
    await until(() => finishHeartbeat);
    let shutdownFinished = false;
    const shutdown = f.hooks.session_shutdown({}, f.ctx).then(() => { shutdownFinished = true; });
    await until(() => finishDetach);
    assert.equal(heartbeatSignal.aborted, true);
    assert.equal(shutdownFinished, false);
    assert.equal(existsSync(f.target().adapter_socket), false);
    finishDetach({ ok: true });
    await shutdown;
    finishHeartbeat({ ok: true, accepted: false });
    await flush();
    assert.equal(f.scheduler.timers.size, 0);
  });

  test('a superseded activation stops retrying and removes only its own endpoint', async t => {
    const f = await managed(t, { respond: () => ({ ok: true, attached: { state: 'superseded' } }) });
    await until(() => f.requests.length);
    await flush();
    assert.equal(f.scheduler.timers.size, 0);
    assert.equal(existsSync(f.target().adapter_socket), false);
  });

  test('RPC, print and JSON children inheriting Herdr never create a bridge', async t => {
    for (const mode of ['rpc', 'print', 'json']) {
      const f = await managed(t, { mode, env: { PI_TEAM_CHILD: '1' } });
      assert.deepEqual(f.requests, []);
      assert.equal(f.scheduler.timers.size, 0);
      assert.equal(existsSync(f.runtime), false);
    }
  });

  test('unsafe runtime directories and symlinks are rejected without chmod or deletion', async t => {
    const { chmod, mkdir, symlink } = await import('node:fs/promises');
    for (const kind of ['unsafe-base', 'unsafe-runtime', 'symlink', 'file']) {
      const f = await managed(t, { prepare: async (base, runtime) => {
        if (kind === 'unsafe-base') await chmod(base, 0o755);
        if (kind === 'unsafe-runtime') { await mkdir(runtime); await chmod(runtime, 0o755); }
        if (kind === 'symlink') await symlink(base, runtime);
        if (kind === 'file') await writeFile(runtime, 'do not delete');
      } });
      await until(() => f.scheduler.timers.size);
      assert.deepEqual(f.requests, []);
      assert.deepEqual(f.notices, []);
      if (kind === 'unsafe-base') assert.equal((await stat(f.directory)).mode & 0o777, 0o755);
      if (kind === 'unsafe-runtime') assert.equal((await stat(f.runtime)).mode & 0o777, 0o755);
      if (kind === 'file') assert.equal(await readFile(f.runtime, 'utf8'), 'do not delete');
      // Runtime setup can recover without restarting the TUI.
      if (kind === 'unsafe-base') {
        await chmod(f.directory, 0o700);
        await f.scheduler.next();
        await until(() => f.requests.some(request => request.event?.type === 'ready'));
      }
    }
  });

  test('replacement before the listening callback is never chmodded or unlinked', async t => {
    const { readdirSync, unlinkSync, writeFileSync } = await import('node:fs');
    const directory = await mkdtemp(join(tmpdir(), 'piv-bind-'));
    const runtime = join(directory, 'pi-voice'), hooks = {};
    const ctx = { mode: 'tui', hasUI: true, cwd: directory, isIdle: () => true, hasPendingMessages: () => false,
      sessionManager: { getSessionId: () => 'binding' }, ui: {} };
    registerVoice({ on: (name, fn) => { hooks[name] = fn; } }, {
      HERDR_ENV: '1', HERDR_SOCKET_PATH: '/fixture', HERDR_PANE_ID: 'p1', XDG_RUNTIME_DIR: directory,
    }, { exchange: () => { throw new Error('no controller'); },
      setTimeout: () => ({ unref() {} }), clearTimeout() {} });
    t.after(async () => { await hooks.session_shutdown({}, ctx); await rm(directory, { recursive: true, force: true }); });
    const start = hooks.session_start({}, ctx);
    const path = join(runtime, readdirSync(runtime)[0]);
    unlinkSync(path);
    writeFileSync(path, 'unrelated', { mode: 0o644 });
    await start;
    await flush();
    await hooks.session_shutdown({}, ctx);
    assert.equal(await readFile(path, 'utf8'), 'unrelated');
    assert.equal((await stat(path)).mode & 0o777, 0o644);
  });

  test('cleanup does not let libuv unlink a replacement inode at the owned pathname', async t => {
    const { unlink } = await import('node:fs/promises');
    const f = await managed(t);
    await until(() => f.scheduler.timers.size);
    const path = f.target().adapter_socket;
    await unlink(path);
    await writeFile(path, 'unrelated replacement');
    await f.hooks.session_shutdown({}, f.ctx);
    await flush();
    assert.equal(await readFile(path, 'utf8'), 'unrelated replacement');
  });

  test('metadata churn cannot postpone the five-second lifetime heartbeat', async t => {
    const f = await managed(t);
    await until(() => f.scheduler.timers.size);
    for (let i = 0; i < 4; i++) {
      f.scheduler.advance(1000);
      await f.hooks.model_select({}, f.ctx);
      await f.scheduler.next();
      await until(() => f.scheduler.timers.size);
    }
    await f.scheduler.next();
    await until(() => f.requests.some(request => request.event?.type === 'heartbeat'));
    assert.equal(f.scheduler.now, 5000);
    for (const { event } of f.requests.filter(request => request.action === 'harness-event')) {
      for (const key of ['bridge_id', 'activation', 'pid', 'session', 'harness']) assert.equal(event[key], f.target()[key]);
    }
  });

  test('missing endpoint is rebound before another heartbeat without changing bridge identity', async t => {
    const { unlink } = await import('node:fs/promises');
    const f = await managed(t);
    await until(() => f.scheduler.timers.size);
    await unlink(f.target().adapter_socket);
    await f.scheduler.next();
    await until(() => f.requests.some(request => request.event?.type === 'heartbeat'));
    assert.equal((await f.request('status')).ok, true);
    assert.equal(f.requests.filter(request => request.action === 'attach').length, 1);
  });

  test('process-global ordinal survives module reload and old shutdown leaves a fresh socket alone', async t => {
    const first = await managed(t);
    await until(() => first.scheduler.timers.size);
    const freshModule = await import(`${extensionPath.href}?reload=${Date.now()}`);
    const second = await managed(t, { register: freshModule.registerVoice });
    await until(() => second.scheduler.timers.size);
    assert.ok(second.target().activation > first.target().activation);
    assert.notEqual(second.target().bridge_id, first.target().bridge_id);
    await first.hooks.session_shutdown({}, first.ctx);
    assert.equal((await second.request('status')).ok, true);
  });
}
