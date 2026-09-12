import assert from 'node:assert/strict';
import test from 'node:test';
import { createTransport } from '../home/config/local-llm/compaction-transport.ts';

function sse(parts) {
  const bytes = new TextEncoder().encode(parts.join('\n\n') + '\n\n');
  return new Response(new ReadableStream({ start(c) {
    for (let i = 0; i < bytes.length; i += 3) c.enqueue(bytes.slice(i, i + 3));
    c.close();
  } }));
}
const data = object => `data: ${JSON.stringify(object)}`;
test('counts the rendered template, including tools, with authenticated requests', async () => {
  const seen = [];
  const transport = createTransport(async (url, options) => {
    seen.push({ url, options });
    return Response.json(url.startsWith('./apply-template') ? { prompt: '<tool>read</tool>Hi' } : { tokens: [1, 2, 3] });
  }, () => ({ Authorization: 'Bearer test-only' }));
  const body = { messages: [{ role: 'user', content: 'Hi' }], tools: [{ name: 'read' }], model: 'a' };
  assert.equal(await transport.count(body), 3);
  assert.deepEqual(JSON.parse(seen[0].options.body).tools, body.tools);
  assert.equal(JSON.parse(seen[1].options.body).content, '<tool>read</tool>Hi');
  assert.equal(seen[0].options.headers.Authorization, 'Bearer test-only');
});
test('reads context and model identity from live properties', async () => {
  const transport = createTransport(async () => Response.json({
    default_generation_settings: { n_ctx: 32768 }, model_path: '/models/changed.gguf', chat_template: 'template',
  }), () => ({}));
  const meta = await transport.metadata('new-model');
  assert.equal(meta.contextSize, 32768);
  assert.ok(meta.identity.includes('/models/changed.gguf'));
});
test('streamed summary handles split UTF-8 and discards reasoning', async () => {
  const transport = createTransport(async () => sse([
    data({ choices: [{ delta: { reasoning_content: 'hidden', content: 'Résumé' } }] }),
    data({ choices: [{ delta: { content: ': preserve the task.' }, finish_reason: 'stop' }] }),
    'data: [DONE]',
  ]), () => ({}));
  assert.equal(await transport.summarise({ messages: [] }), 'Résumé: preserve the task.');
});
test('truncated summaries are rejected', async () => {
  const transport = createTransport(async () => sse([
    data({ choices: [{ delta: { content: 'unfinished' }, finish_reason: 'length' }] }), 'data: [DONE]',
  ]), () => ({}));
  await assert.rejects(transport.summarise({ messages: [] }), /truncated/i);
});
test('a dropped stream cannot be committed as a summary', async () => {
  const transport = createTransport(async () => sse([
    data({ choices: [{ delta: { content: 'partial' } }] }),
  ]), () => ({}));
  await assert.rejects(transport.summarise({ messages: [] }), /incomplete/i);
});
test('Stop signal reaches every request and prevents continuation', async () => {
  const controller = new AbortController();
  const transport = createTransport(async (_url, options) => {
    assert.equal(options.signal, controller.signal);
    controller.abort();
    return Response.json({ prompt: 'x' });
  }, () => ({}), controller.signal);
  await assert.rejects(transport.count({ messages: [] }), { name: 'AbortError' });
});
test('server errors expose a clear failure, not a silent history truncation', async () => {
  const transport = createTransport(async () => Response.json({ error: { message: 'bad template' } }, { status: 400 }), () => ({}));
  await assert.rejects(transport.count({ messages: [] }), /bad template/);
});
test('image input is not silently undercounted as text', async () => {
  const transport = createTransport(async () => { throw Error('must not call'); }, () => ({}));
  await assert.rejects(transport.count({ messages: [{ role: 'user', content: [{ type: 'image_url', image_url: { url: 'data:x' } }] }] }), /text-only/i);
});
