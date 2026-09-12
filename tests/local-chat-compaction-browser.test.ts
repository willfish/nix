import assert from 'node:assert/strict';
import test from 'node:test';
import { compactForChatWith } from '../home/config/local-llm/compaction-browser.ts';

const message = (role, content) => ({ role, content });
const history = (n = 20) => Array.from({ length: n }, (_, i) => [
  message('user', `Question ${i}: ${'detail '.repeat(60)}`),
  message('assistant', `Answer ${i}: ${'finding '.repeat(60)}`),
]).flat();

function sse(text) {
  return new Response([
    `data: ${JSON.stringify({ choices: [{ delta: { content: text } }] })}`,
    `data: ${JSON.stringify({ choices: [{ delta: {}, finish_reason: 'stop' }] })}`,
    'data: [DONE]',
  ].join('\n\n') + '\n\n', { headers: { 'content-type': 'text/event-stream' } });
}

async function defaultFetch(url, options) {
  if (String(url).startsWith('./props')) {
    return Response.json({
      default_generation_settings: { n_ctx: 4096 },
      model_path: '/models/a.gguf',
      chat_template: 'template',
    });
  }
  if (String(url).startsWith('./apply-template')) {
    return Response.json({ prompt: JSON.stringify(JSON.parse(options.body).messages) });
  }
  if (String(url).startsWith('./tokenize')) {
    const tokens = Array.from(
      { length: Math.ceil(JSON.parse(options.body).content.length / 4) },
      (_, i) => i,
    );
    return Response.json({ tokens });
  }
  if (String(url).startsWith('./v1/chat/completions')) {
    return sse('Continuity summary: retain the task and findings.');
  }
  throw new Error(`unexpected ${url}`);
}

function ports(overrides = {}) {
  const toasts = [];
  const conversations = new Map();
  const impl = {
    fetch: defaultFetch,
    headers: () => ({ Authorization: 'Bearer test-only' }),
    load: async (id) => conversations.get(id),
    save: async (id, cache) => { conversations.set(id, cache); },
    toast: {
      loading: (text, opts) => toasts.push({ kind: 'loading', text, ...opts }),
      success: (text, opts) => toasts.push({ kind: 'success', text, ...opts }),
      dismiss: (id) => toasts.push({ kind: 'dismiss', id }),
    },
    ...overrides,
  };
  return { impl, toasts, conversations };
}

test('short conversation requests pass through without toasts or saves', async () => {
  const { impl, toasts, conversations } = ports();
  const request = { model: 'a', messages: [message('user', 'Hello')] };
  assert.deepEqual(await compactForChatWith(impl, request, 'chat-1'), request);
  assert.equal(toasts.length, 0);
  assert.equal(conversations.size, 0);
});

test('compaction toasts, persists per conversation, and keeps the current request', async () => {
  const { impl, toasts, conversations } = ports();
  const request = { model: 'a', messages: [...history(), message('user', 'Finish the task.')] };
  const result = await compactForChatWith(impl, request, 'chat-2');
  assert.ok(result.messages.length < request.messages.length);
  assert.deepEqual(result.messages.at(-1), request.messages.at(-1));
  assert.equal(conversations.has('chat-2'), true);
  assert.equal(toasts[0].kind, 'loading');
  assert.equal(toasts[0].id, 'local-compaction-chat-2');
  assert.equal(toasts.at(-1).kind, 'success');
  assert.match(toasts.at(-1).text, /Full chat history is retained/);
});

test('a failed summary dismisses the loading toast and does not save', async () => {
  const { impl, toasts, conversations } = ports({
    fetch: async (url, options) => {
      if (String(url).startsWith('./v1/chat/completions')) {
        return new Response(JSON.stringify({ error: { message: 'backend down' } }), { status: 500 });
      }
      return defaultFetch(url, options);
    },
  });
  await assert.rejects(
    compactForChatWith(impl, { model: 'a', messages: history() }, 'chat-3'),
    /backend down/,
  );
  assert.equal(conversations.size, 0);
  assert.ok(toasts.some((event) => event.kind === 'loading' && event.id === 'local-compaction-chat-3'));
  assert.ok(toasts.some((event) => event.kind === 'dismiss' && event.id === 'local-compaction-chat-3'));
});

test('abort during save dismisses a shown toast', async () => {
  const controller = new AbortController();
  const { impl, toasts } = ports({
    save: async () => {
      controller.abort();
      controller.signal.throwIfAborted();
    },
  });
  await assert.rejects(
    compactForChatWith(impl, { model: 'a', messages: history() }, 'chat-4', controller.signal),
    /aborted/,
  );
  assert.ok(toasts.some((event) => event.kind === 'loading'));
  assert.ok(toasts.some((event) => event.kind === 'dismiss' && event.id === 'local-compaction-chat-4'));
});
