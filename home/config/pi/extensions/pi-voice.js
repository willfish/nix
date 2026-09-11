// Session-scoped local voice bridge using Pi's supported extension API.
import { chmodSync, lstatSync, mkdirSync, realpathSync, unlinkSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { join, resolve } from 'node:path';
import net from 'node:net';

const MAX_FRAME = 256 * 1024;
const RETRY_MS = [250, 500, 1000, 2000, 5000];
const HEARTBEAT_MS = 5000;
const activationCounter = Symbol.for('pi.voice.activation');

function exchange(path, request, timeout = 1000, signal) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(path);
    // Final detach must keep the event loop alive after Pi stops terminal input.
    const keepAlive = request.action === 'detach';
    if (!keepAlive) socket.unref();
    socket.setEncoding('utf8');
    let data = '';
    let finished = false;
    const finish = (error, result) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
      socket.destroy();
      if (error) reject(error); else resolve(result);
    };
    const abort = () => finish(new Error('Voice exchange cancelled'));
    const timer = setTimeout(() => finish(new Error('Voice unavailable')), timeout);
    if (!keepAlive) timer.unref();
    signal?.addEventListener('abort', abort, { once: true });
    if (signal?.aborted) { abort(); return; }
    socket.on('error', error => finish(error));
    socket.on('connect', () => socket.write(`${JSON.stringify(request)}\n`));
    socket.on('data', chunk => {
      data += chunk;
      if (Buffer.byteLength(data) > MAX_FRAME) { finish(new Error('Invalid voice response')); return; }
      if (data.includes('\n')) {
        try { finish(null, JSON.parse(data.split('\n', 1)[0])); } catch (error) { finish(error); }
      }
    });
    socket.on('end', () => finish(new Error('Voice connection closed')));
    socket.on('close', () => finish(new Error('Voice connection closed')));
  });
}

function checkedText(value) {
  if (typeof value !== 'string' || Buffer.byteLength(value) > MAX_FRAME
    || !/[\p{L}\p{N}]/u.test(value)
    || /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f]/u.test(value)) {
    throw new Error('Invalid dictation');
  }
  return value.trim();
}

function observed(path) {
  try { return lstatSync(path); } catch (error) { if (error.code !== 'ENOENT') throw error; }
}
const sameInode = (a, b) => a && b && a.dev === b.dev && a.ino === b.ino;
function privateRuntime(base) {
  // Never repair permissions or follow an unexpected symlink on an existing directory.
  const check = path => {
    const stat = lstatSync(path);
    if (!stat.isDirectory() || stat.uid !== process.getuid()
      || (stat.mode & 0o077) || realpathSync(path) !== resolve(path)) {
      throw new Error('Unsafe Pi voice runtime directory');
    }
  };
  check(base);
  const runtime = join(base, 'codex-voice');
  try { mkdirSync(runtime, { mode: 0o700 }); } catch (error) { if (error.code !== 'EEXIST') throw error; }
  check(runtime);
  return runtime;
}

export function registerVoice(pi, env = process.env, options = {}) {
  const legacy = Boolean(env.AGENT_VOICE_TOKEN);
  const harness = legacy ? env.AGENT_VOICE_KIND : (env.PI_VOICE_HARNESS || 'pi');
  if (!['pi', 'qwen-pi'].includes(harness)) return;
  if (legacy) {
    if (!env.AGENT_VOICE_ADAPTER_SOCKET || !env.AGENT_VOICE_SOCKET
      || Number(env.AGENT_VOICE_LAUNCH_PID) !== process.ppid) return;
  } else if (env.HERDR_ENV !== '1' || !env.HERDR_SOCKET_PATH || !env.HERDR_PANE_ID) return;
  const base = env.XDG_RUNTIME_DIR || `/run/user/${process.getuid()}`;
  const controller = env.AGENT_VOICE_SOCKET || join(base, 'codex-voice', 'control.sock');
  const transport = options.exchange || exchange;
  const schedule = options.setTimeout || setTimeout;
  const cancelTimer = options.clearTimeout || clearTimeout;
  const random = options.random || Math.random;
  const now = options.now || (() => performance.now());
  let active;

  function activate(ctx) {
    const activation = globalThis[activationCounter] = (globalThis[activationCounter] ?? 0) + 1;
    const bridge_id = randomUUID();
    const session = ctx.sessionManager.getSessionId();
    const path = legacy ? env.AGENT_VOICE_ADAPTER_SOCKET : join(base, 'codex-voice', `pi-${process.pid}-${bridge_id}.sock`);
    let token = legacy ? env.AGENT_VOICE_TOKEN : undefined;
    let context = ctx, server, owned, timer, binding, closeResult;
    let closed = false, armed = false, staged = '', waiting = false, lastReply;
    let retry = 0, inflight = false, acknowledged = false, heartbeatAt = 0;
    const pending = new Map();
    const abort = new AbortController();
    const connections = new Set();
    const current = () => !closed && active === instance;
    const ready = () => Boolean(context?.hasUI && !waiting && context.isIdle() && !context.hasPendingMessages());
    const identity = () => ({ harness, pid: process.pid, session, ...(legacy ? {} : { bridge_id, activation }) });
    const metadata = () => ({ model: context.model?.id || '', thinking: context.thinkingLevel ?? pi.getThinkingLevel?.() ?? '',
      team_child: env.PI_TEAM_CHILD === '1' });
    const status = () => ({ ...identity(), ready: ready(),
      state: waiting ? 'blocked' : ready() ? 'idle' : 'working', draft: armed });
    const eventRequest = (type, fields = {}) => ({ action: 'harness-event', token,
      event: { ...status(), cwd: context.cwd, adapter_socket: path, ...metadata(), ...fields, ...identity(), type } });
    const later = delay => {
      if (!current()) return;
      cancelTimer(timer);
      timer = schedule(() => { timer = undefined; void pump(); }, delay);
      timer?.unref?.();
    };
    const retryLater = () => {
      const delay = RETRY_MS[Math.min(retry++, RETRY_MS.length - 1)];
      later(delay === 5000 ? delay + Math.floor(random() * 250) : delay);
    };
    const lostToken = () => {
      token = undefined;
      acknowledged = false;
      armed = false;
      staged = '';
      // Registration loss disarms input, not output. Pending events remain scoped
      // to this activation and agent_start invalidates the previous final reply.
    };
    async function pump() {
      if (!current() || inflight) return;
      inflight = true;
      let success = false;
      try {
        await bind();
        if (!current()) return;
        let response;
        if (!token) {
          response = await transport(controller, { action: 'attach', target: {
            pane: env.HERDR_PANE_ID, socket: env.HERDR_SOCKET_PATH, adapter_socket: path,
            ...identity(), ...metadata(),
          } }, 1000, abort.signal);
          if (!current()) return;
          if (response?.attached?.state === 'superseded') { close(); return; }
          if (response?.ok && ['connecting', 'ready'].includes(response.attached?.state) && typeof response.attached.token === 'string'
            && response.attached.token) {
            token = response.attached.token;
            acknowledged = false;
          } else return;
        }
        const type = !acknowledged ? 'ready' : now() >= heartbeatAt ? 'heartbeat'
          : pending.keys().next().value || 'heartbeat';
        const fields = pending.get(type) || {};
        const sentToken = token;
        response = await transport(controller, eventRequest(type, fields), 1000, abort.signal);
        if (!current() || sentToken !== token) return;
        if (response?.attached?.state === 'superseded') { close(); return; }
        if (response?.accepted === false) { lostToken(); return; }
        if (!response?.ok || response.accepted !== true) return;
        // A newer event (or turn) can replace this entry during the exchange.
        // Only remove the exact event the controller has acknowledged.
        if (pending.get(type) === fields) pending.delete(type);
        acknowledged = true;
        if (type === 'ready' || type === 'heartbeat') heartbeatAt = now() + HEARTBEAT_MS;
        retry = 0;
        success = true;
      } catch { /* Controller discovery and transport failures are quiet and retryable. */ }
      finally {
        inflight = false;
        if (current()) {
          if (success) later(pending.size ? 0 : Math.max(0, heartbeatAt - now()));
          else retryLater();
        }
      }
    }
    async function emitLegacy(type, fields = {}) {
      const request = eventRequest(type, fields);
      const deadline = performance.now() + 2000;
      try {
        do {
          const remaining = deadline - performance.now();
          if (remaining <= 0) return;
          const response = await transport(controller, request, Math.max(1, Math.min(1000, remaining)));
          if (type !== 'session' || !response?.ok || response.accepted !== false) return;
          if (deadline - performance.now() <= 100) return;
          // Popen can start the extension before its launcher registers the token.
          await new Promise(resolve => setTimeout(resolve, 100));
        } while (current());
      } catch { /* Voice service downtime must not interrupt Pi. */ }
    }
    const emit = (type, fields = {}) => {
      if (legacy) return emitLegacy(type, fields);
      if (!current()) return;
      // Coalesce metadata/activity while retaining only the latest final reply
      // until acknowledged, including during reattachment. Never queue input operations.
      if (type === 'busy' || type === 'settled') { pending.delete('busy'); pending.delete('settled'); }
      pending.set(type, fields);
      if (!inflight) later(0);
    };
    const handle = request => {
      if (!current() || !token || request.token !== token || request.session !== session
        || (!legacy && (request.bridge_id !== bridge_id || request.activation !== activation
          || request.pid !== process.pid || request.harness !== harness))) {
        throw new Error('Voice selection does not match this Pi session; rebind it');
      }
      if (request.command === 'status') return status();
      if (!ready()) throw new Error('Pi is busy or waiting for an interaction');
      if (request.command === 'stage') {
        const text = checkedText(request.text);
        const editor = context.ui.getEditorText();
        const next = editor ? `${editor}${/\s$/u.test(editor) ? '' : ' '}${text}` : text;
        context.ui.setEditorText(next);
        staged = next;
        armed = true;
        return status();
      }
      if (request.command === 'submit') {
        if (!armed) throw new Error('No voice dictation is ready to send');
        const text = context.ui.getEditorText();
        if (text !== staged && request.allow_edited !== true) {
          armed = false;
          throw new Error('Pi prompt changed; review and send it from Pi');
        }
        checkedText(text);
        armed = false;
        staged = '';
        context.ui.setEditorText('');
        try { pi.sendUserMessage(text); } catch {
          const error = new Error('Could not confirm submission; check Pi');
          error.uncertain = true;
          throw error;
        }
        return status();
      }
      throw new Error('Unknown Pi voice command');
    };
    function closeServer() {
      for (const socket of connections) socket.destroy();
      if (!server || (binding && !owned)) return;
      const old = server;
      server = undefined;
      let now;
      try { now = observed(path); } catch { old.unref(); owned = undefined; return; }
      // libuv unlinks Unix listen paths on close, even if another inode replaced
      // them. In that exceptional case abandon the unref'd handle, rejecting all
      // requests, rather than let close delete a file we do not own.
      if (!now || sameInode(owned, now)) {
        old.close();
        if (sameInode(owned, observed(path))) unlinkSync(path);
      } else old.unref();
      owned = undefined;
    }
    function close() {
      if (closed) return closeResult;
      closed = true;
      armed = false;
      staged = '';
      lastReply = undefined;
      pending.clear();
      cancelTimer(timer);
      abort.abort();
      closeServer();
      if (!legacy && token) {
        // Give the explicit detach one bounded exchange before Pi exits. Never
        // await discovery, retry detach, or retain the aborted attachment request.
        closeResult = Promise.resolve().then(() => transport(controller, {
          action: 'detach', token, bridge_id, activation, pid: process.pid,
        }, 1000)).catch(() => {});
      }
      return closeResult;
    }
    function bind() {
      if (owned && sameInode(owned, observed(path))) return Promise.resolve();
      if (owned) closeServer();
      if (binding) return binding;
      binding = new Promise((resolveBind, reject) => {
        try {
          if (!legacy) privateRuntime(base);
          if (observed(path)) throw new Error('Pi voice socket is already in use');
          const local = net.createServer(socket => {
            if (!current() || server !== local) { socket.destroy(); return; }
            socket.setEncoding('utf8');
            connections.add(socket);
            socket.on('close', () => connections.delete(socket));
            socket.on('error', () => {});
            socket.setTimeout(2000, () => socket.destroy());
            let data = '', handled = false;
            socket.on('data', chunk => {
              if (handled) return;
              data += chunk;
              if (Buffer.byteLength(data) > MAX_FRAME) { socket.destroy(); return; }
              if (!data.includes('\n')) return;
              handled = true;
              let response;
              try { response = { ok: true, result: handle(JSON.parse(data.split('\n', 1)[0])) }; }
              catch (error) { response = { ok: false, error: error.message, uncertain: Boolean(error.uncertain) }; }
              socket.end(`${JSON.stringify(response)}\n`);
            });
          });
          server = local;
          local.unref();
          local.on('error', error => {
            if (server === local) server = undefined;
            local.close();
            reject(error);
          });
          local.listen(path, () => {
            try {
              if (!sameInode(owned, observed(path))) throw new Error('Pi voice endpoint was replaced');
              if (!current()) closeServer();
              resolveBind();
            } catch (error) { closeServer(); reject(error); }
          });
          // Unix listen binds synchronously, but its callback runs on nextTick.
          // Capture ownership before any extension callback can replace the path.
          if (local.listening) {
            const bound = lstatSync(path);
            if (!bound.isSocket() || bound.uid !== process.getuid()) throw new Error('Unsafe Pi voice endpoint');
            owned = bound;
            chmodSync(path, 0o600);
          }
        } catch (error) { closeServer(); reject(error); }
      }).finally(() => { binding = undefined; });
      return binding;
    }
    const instance = { close,
      async start() {
        if (!legacy) { void pump(); return; }
        try { await bind(); if (current()) await emit('session', { ready: ready(), name: pi.getSessionName?.() || '' }); }
        catch (error) { close(); ctx.ui.notify(`Voice unavailable: ${error.message}`, 'warning'); }
      },
      input() { armed = false; staged = ''; },
      agent_start(_event, ctx) { context = ctx; armed = false; staged = ''; lastReply = undefined; pending.delete('reply'); return emit('busy'); },
      agent_end(event, ctx) {
        context = ctx;
        lastReply = undefined;
        if (env.PI_TEAM_CHILD === '1') return;
        const last = event.messages?.at(-1);
        if (last?.role === 'assistant' && last.stopReason === 'stop') {
          const text = last.content.filter(item => item.type === 'text').map(item => item.text).join('\n');
          if (text.trim() && Buffer.byteLength(text) < MAX_FRAME - 4096) lastReply = { text, turn: ctx.sessionManager.getLeafId() };
        }
      },
      async agent_settled(_event, ctx) {
        context = ctx;
        if (!ready()) return;
        await emit('settled');
        if (current() && ready() && lastReply) {
          const reply = lastReply;
          lastReply = undefined;
          await emit('reply', reply);
        }
      },
      ui_prompt_start(_event, ctx) { context = ctx; waiting = true; return emit('busy', { state: 'blocked' }); },
      ui_prompt_end(_event, ctx) { context = ctx; waiting = false; return emit(ready() ? 'settled' : 'busy'); },
      metadata(_event, ctx) { context = ctx; return emit('metadata'); },
      async shutdown() {
        const detached = close();
        if (legacy) await emitLegacy('shutdown'); else await detached;
      },
    };
    return instance;
  }
  pi.on('session_start', (_event, ctx) => {
    active?.close();
    active = undefined;
    if (!ctx.hasUI || (!legacy && ctx.mode !== 'tui')) return;
    active = activate(ctx);
    return active.start();
  });
  for (const name of ['input', 'agent_start', 'agent_end', 'agent_settled', 'ui_prompt_start', 'ui_prompt_end']) {
    pi.on(name, (event, ctx) => active?.[name](event, ctx));
  }
  for (const name of ['model_select', 'thinking_level_select', 'session_info_changed']) {
    pi.on(name, (event, ctx) => active?.metadata(event, ctx));
  }
  pi.on('session_shutdown', () => {
    const old = active;
    active = undefined;
    return old?.shutdown();
  });
}

export default function voice(pi) { registerVoice(pi); }
