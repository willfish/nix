// Session-scoped local voice bridge using Pi's supported extension API.
import { chmod, lstat, unlink } from 'node:fs/promises';
import net from 'node:net';

const MAX_FRAME = 256 * 1024;

function exchange(path, request, timeout = 1000) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(path);
    socket.setEncoding('utf8');
    let data = '';
    const timer = setTimeout(() => socket.destroy(new Error('Voice unavailable')), timeout);
    socket.once('close', () => clearTimeout(timer));
    socket.on('error', reject);
    socket.on('connect', () => socket.write(`${JSON.stringify(request)}\n`));
    socket.on('data', (chunk) => {
      data += chunk;
      if (data.length > MAX_FRAME) socket.destroy(new Error('Invalid voice response'));
      if (data.includes('\n')) {
        socket.end();
        try { resolve(JSON.parse(data.split('\n', 1)[0])); } catch (error) { reject(error); }
      }
    });
    socket.on('end', () => reject(new Error('Voice connection closed')));
  });
}

function checkedText(value) {
  if (typeof value !== 'string' || !/[\p{L}\p{N}]/u.test(value)
    || /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f-\x9f]/u.test(value)) {
    throw new Error('Invalid dictation');
  }
  return value.trim();
}

export function registerVoice(pi, env = process.env) {
  const token = env.AGENT_VOICE_TOKEN;
  const path = env.AGENT_VOICE_ADAPTER_SOCKET;
  const controller = env.AGENT_VOICE_SOCKET;
  const harness = env.AGENT_VOICE_KIND;
  if (!token || !path || !controller || !['pi', 'qwen-pi'].includes(harness)
    || Number(env.AGENT_VOICE_LAUNCH_PID) !== process.ppid) return;

  let context;
  let server;
  let ownsSocket = false;
  let session;
  let armed = false;
  let staged = '';
  let waiting = false;
  let lastReply;
  let closed = false;
  const connections = new Set();
  const ready = () => Boolean(context?.hasUI && !waiting && context.isIdle()
    && !context.hasPendingMessages());
  const emit = async (type, fields = {}) => {
    if (!session) return;
    const request = {
      action: 'harness-event', token,
      event: { harness, type, session, pid: process.pid, cwd: context.cwd,
        adapter_socket: path, ...fields },
    };
    const deadline = performance.now() + 2000;
    try {
      do {
        const remaining = deadline - performance.now();
        if (remaining <= 0) return;
        const response = await exchange(controller, request, Math.max(1, Math.min(1000, remaining)));
        if (type !== 'session' || !response?.ok || response.accepted !== false) return;
        if (deadline - performance.now() <= 100) return;
        // Popen can start the extension before its launcher registers the token.
        await new Promise((resolve) => setTimeout(resolve, 100));
      } while (!closed);
    } catch { /* Voice service downtime must not interrupt Pi. */ }
  };
  const status = () => ({ session, pid: process.pid, ready: ready(),
    state: waiting ? 'blocked' : ready() ? 'idle' : 'working', draft: armed });
  const handle = (request) => {
    if (closed || request.token !== token || request.session !== session) {
      throw new Error('Voice selection does not match this Pi session; rebind it');
    }
    if (request.command === 'status') return status();
    if (!ready()) throw new Error('Pi is busy or waiting for an interaction');
    if (request.command === 'stage') {
      const text = checkedText(request.text);
      const current = context.ui.getEditorText();
      const next = current ? `${current}${/\s$/u.test(current) ? '' : ' '}${text}` : text;
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
        // Once invoked, do not retry: Pi may already have accepted the message.
        const error = new Error('Could not confirm submission; check Pi');
        error.uncertain = true;
        throw error;
      }
      return status();
    }
    throw new Error('Unknown Pi voice command');
  };
  const close = async () => {
    closed = true;
    for (const socket of connections) socket.destroy();
    if (server) {
      const old = server;
      server = undefined;
      await new Promise((resolve) => old.close(resolve));
      if (ownsSocket) await unlink(path).catch(() => {});
      ownsSocket = false;
    }
  };
  pi.on('session_start', async (_event, ctx) => {
    context = ctx;
    session = ctx.sessionManager.getSessionId();
    if (!ctx.hasUI) return;
    armed = false;
    staged = '';
    waiting = false;
    lastReply = undefined;
    try {
      await lstat(path).then(() => {
        throw new Error('Pi voice socket is already in use');
      }, (error) => { if (error.code !== 'ENOENT') throw error; });
    } catch (error) {
      ctx.ui.notify(`Voice unavailable: ${error.message}`, 'warning');
      return;
    }
    closed = false;
    server = net.createServer((socket) => {
      socket.setEncoding('utf8');
      connections.add(socket);
      socket.on('close', () => connections.delete(socket));
      socket.on('error', () => {});
      socket.setTimeout(2000, () => socket.destroy());
      let data = '';
      let handled = false;
      socket.on('data', (chunk) => {
        if (handled) return;
        data += chunk;
        if (data.length > MAX_FRAME) { socket.destroy(); return; }
        if (!data.includes('\n')) return;
        handled = true;
        let response;
        try { response = { ok: true, result: handle(JSON.parse(data.split('\n', 1)[0])) }; }
        catch (error) { response = { ok: false, error: error.message, uncertain: Boolean(error.uncertain) }; }
        socket.end(`${JSON.stringify(response)}\n`);
      });
    });
    try {
      await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(path, resolve);
      });
      ownsSocket = true;
      await chmod(path, 0o600);
      await emit('session', { ready: ready(), name: pi.getSessionName?.() || '' });
    } catch (error) {
      await close();
      ctx.ui.notify(`Voice unavailable: ${error.message}`, 'warning');
    }
  });
  pi.on('input', () => { armed = false; staged = ''; });
  pi.on('agent_start', async (_event, ctx) => {
    context = ctx;
    armed = false;
    staged = '';
    lastReply = undefined;
    await emit('busy');
  });
  pi.on('agent_end', (event, ctx) => {
    context = ctx;
    lastReply = undefined;
    const last = event.messages?.at(-1);
    if (last?.role === 'assistant' && last.stopReason === 'stop') {
      const text = last.content.filter((item) => item.type === 'text').map((item) => item.text).join('\n');
      if (text.trim()) lastReply = { text, turn: ctx.sessionManager.getLeafId() };
    }
  });
  pi.on('agent_settled', async (_event, ctx) => {
    context = ctx;
    if (!ready()) return;
    await emit('settled');
    if (lastReply) {
      const reply = lastReply;
      lastReply = undefined;
      await emit('reply', reply);
    }
  });
  pi.on('ui_prompt_start', async (_event, ctx) => {
    context = ctx;
    waiting = true;
    await emit('busy', { state: 'blocked' });
  });
  pi.on('ui_prompt_end', async (_event, ctx) => {
    context = ctx;
    waiting = false;
    await emit(ready() ? 'settled' : 'busy');
  });
  pi.on('session_shutdown', async () => {
    await close();
    await emit('shutdown');
  });
}

export default function voice(pi) { registerVoice(pi); }
